
#include <cassert>
#include <ctype.h>
#include "xdrc_internal.h"

using std::endl;

std::unordered_map<string, string> xdr_type_map = {
  {"unsigned", "std::uint32_t"},
  {"int", "std::int32_t"},
  {"unsigned hyper", "std::uint64_t"},
  {"hyper", "std::int64_t"},
};

string
map_type(const string &s)
{
  auto t = xdr_type_map.find(s);
  if (t == xdr_type_map.end())
    return s;
  return t->second;
}

string
map_tag(const string &s)
{
  assert (!s.empty());
  if (s == "TRUE")
    return "true";
  if (s == "FALSE")
    return "false";
  if (s[0] == '-')
    // Stuff should get implicitly converted to unsigned anyway, but
    // this avoids clang++ warnings.
    return string("std::uint32_t(") + s + ")";
  else
    return s;
}

string
map_case(const string &s)
{
  if (s.empty())
    return "default:";
  return "case " + map_tag(s) + ":";
}

namespace {

indenter nl;

string
guard_token()
{
  string in;
  if (!output_file.empty())
    in = output_file;
  else {
    size_t r = input_file.rfind('/');
    if (r != string::npos)
      in = input_file.substr(r+1);
    else
      in = input_file;
    r = in.size();
    if (r >= 2 && in.substr(r-2) == ".x")
      in.back() = 'h';
  }

  string ret = "__XDR_";
  for (char c : in)
    if (isalnum(c))
      ret += toupper(c);
    else
      ret += "_";
  ret += "_INCLUDED__";
  return ret;
}

string
decl_type(const rpc_decl &d)
{
  string type = map_type(d.type);
  if (type == "string" || type == "opaque") {
    switch (d.qual) {
    case rpc_decl::ARRAY:
      return string("std::array<char,") + d.bound + ">";
    case rpc_decl::VEC:
      return string("xdr::xstring<") + d.bound + ">";
    default:
      assert(!"Invalid size for opaque/string");
    }
  }

  switch (d.qual) {
  case rpc_decl::PTR:
    return string("std::unique_ptr<") + type + ">";
  case rpc_decl::ARRAY:
    return string("std::array<") + type + "," + d.bound + ">";
  case rpc_decl::VEC:
    return string("std::vector<") + type + ">";
  default:
    return type;
  }
}

inline string
id_space(const string &s)
{
  return s.empty() ? s : s + ' ';
}

void
gen(std::ostream &os, const rpc_struct &s)
{
  os << "struct " << id_space(s.id) << '{';
  ++nl;
  for(auto d : s.decls)
    os << nl << decl_type(d) << ' ' << d.id << ';';
  os << nl.close << "}";
  if (!s.id.empty())
    os << ';';
}

void
gen(std::ostream &os, const rpc_enum &e)
{
  os << "enum " << id_space(e.id) << ": std::uint32_t {";
  ++nl;
  for (rpc_const c : e.tags)
    if (c.val.empty())
      os << nl << c.id << ',';
    else
      os << nl << c.id << " = " << c.val << ',';
  os << nl.close << "}";
  if (!e.id.empty())
    os << ';';
}

string
pswitch(const rpc_union &u, string id = string())
{
  if (id.empty())
    id = u.tagid + '_';
  string ret = "switch (";
  if (u.tagtype == "bool")
    ret += "std::uint32_t{" + id + "}";
  else
    ret += id;
  ret += ") {";
  return ret;
}

void
gen(std::ostream &os, const rpc_union &u)
{
  os << "class " << u.id << " {"
    //<< nl.open << map_type(u.tagtype) << ' ' << u.tagid << "_;"
     << nl.open << "std::uint32_t " << u.tagid << "_;"
     << nl << "union {";
  ++nl;
  for (rpc_ufield f : u.fields)
    if (f.field.type != "void")
      os << nl << decl_type(f.field) << ' ' << f.field.id << "_;";
  os << nl.close << "};" << endl;

  os << nl.outdent << "public:";
  os << nl << "static_assert (sizeof (" << u.tagtype << ") <= 4,"
    " \"union discriminant must be 4 bytes\");";

  if (u.hasdefault)
    os << nl << "static constexpr bool _tag_is_valid("
       << u.tagtype << ") { return true; }";
  else {
    os << nl << "static bool _tag_is_valid(" << u.tagtype << " t) {";
    os << nl.open << pswitch(u, "t");
    for (rpc_ufield f : u.fields)
      for (string name : f.cases)
	os << nl << map_case(name);
    os << nl << "  return true;"
       << nl << "}"
       << nl << "return false;"
       << nl.close << "}";
  }

  os << nl << "static const char *_name_of_field(std::uint32_t t) {"
     << nl.open << "switch (t) {";
  for (rpc_ufield f : u.fields) {
    for (string c : f.cases)
      os << nl << map_case(c);
    if (f.field.type == "void")
      os << nl << "  return nullptr;";
    else
      os << nl << "  return \"" << f.field.id << "\";";
  }
  os << nl << "}";
  if (!u.hasdefault)
    os << nl << "return nullptr;";
  os << nl.close << "}" << endl;

  // _apply_to_selected
  for (auto constkw : {"", "const "}) {
    os << nl << "template<typename F>"
       << nl << "xdr::result_type_or_void<F> _apply_to_selected(F &_f) "
       << constkw << "{"
       << nl.open << pswitch(u);
    for (rpc_ufield f : u.fields) {
      for (string c : f.cases)
	os << nl << map_case(c);
      if (f.field.type == "void")
	os << nl << "  return _f();";
      else
	os << nl << "  return _f(" << f.field.id << "_);";
    }
    os << nl << "}"
       << nl.close << "}";
  }
  os << endl;

  // Constructor
  os << nl << u.id << "(" << map_type(u.tagtype) << " _t = "
     << map_type(u.tagtype) << "{}) : " << u.tagid << "_(_t) {"
     << nl.open << "_apply_to_selected(xdr::case_constructor);"
     << nl.close << "}"
     << nl << "~" << u.id << "() { _apply_to_selected(xdr::case_destroyer); }"
     << endl;

  // Tag getter/setter
  os << nl << map_type(u.tagtype) << ' ' << u.tagid << "() const { return "
     << map_type(u.tagtype) << "(" << u.tagid << "_); }";
  os << nl << "void set_" << u.tagid << "(" << u.tagtype << " _t) {"
     << nl.open << "if (std::uint32_t(_t) != " << u.tagid << "_) {"
     << nl.open << "_apply_to_selected(xdr::case_destroyer);"
     << nl << u.tagid << "_ = _t;"
     << nl << "_apply_to_selected(xdr::case_constructor);"
     << nl.close << "}"
     << nl.close << "}" << endl;

  // Field accessors
  for (auto f = u.fields.cbegin(); f != u.fields.cend(); f++) {
    if (f->field.type == "void")
      continue;
    os << nl << decl_type(f->field) << " *_" << f->field.id << "() {";
    ++nl;
    if (f->hasdefault) {
      if (u.fields.size() == 1)
	os << nl << "return &" << f->field.id << "_;";
      else {
	os << nl << pswitch(u);
	for (auto ff = u.fields.cbegin(); ff != u.fields.cend(); ff++) {
	  if (ff == f)
	    continue;
	  for (string c : ff->cases)
	    os << nl << map_case(c);
	}
	os << nl << "  return nullptr;"
	   << nl << "default:"
	   << nl << "  return &" << f->field.id << "_;"
	   << nl << "}";
      }
    }
    else if (f->cases.size() < 3) {
      string t = u.tagid + "_";
      os << nl << "return " << t << " == " << map_tag(f->cases[0]);
      for (size_t i = 1; i < f->cases.size(); i++)
	os << " || " << t << " == " << map_tag(f->cases[i]);
      os << " ? &" << f->field.id << "_" << " : nullptr;";
    }
    else {
      os << nl << pswitch(u);
      for (string c : f->cases)
	os << nl << map_case(c);
      os << nl << "  return &" << f->field.id << "_;"
	 << nl << "default:"
	 << nl << "  return nullptr;"
	 << nl << "}";
    }
    os << nl.close << "}";
  }

  os << nl.close << "}";
  if (!u.id.empty())
    os << ';';
}

}

void
gen_hh(std::ostream &os)
{
  os << "// -*-C++-*-"
     << nl << "// Automatically generated from " << input_file << '.' << endl;

  string gtok = guard_token();
  os << nl << "#ifndef " << gtok
     << nl << "#define " << gtok << " 1" << endl
     << nl << "#include <xdrc/types.h>";

  int last_type = -1;

  for (auto s : symlist) {
    switch(s.type) {
    case rpc_sym::CONST:
    case rpc_sym::TYPEDEF:
    case rpc_sym::LITERAL:
      if (s.type != last_type)
      // cascade
    default:
	os << endl;
    }
    os << nl;
    switch(s.type) {
    case rpc_sym::CONST:
      os << "constexpr std::uint32_t " << s.sconst->id << " = "
	 << s.sconst->val << ';';
      break;
    case rpc_sym::STRUCT:
      gen(os, *s.sstruct);
      break;
    case rpc_sym::UNION:
      gen(os, *s.sunion);
      break;
    case rpc_sym::ENUM:
      gen(os, *s.senum);
      break;
    case rpc_sym::TYPEDEF:
      os << "using " << s.stypedef->id << " = "
	 << decl_type(*s.stypedef) << ';';
      break;
    case rpc_sym::PROGRAM:
      /* need some action */
      break;
    case rpc_sym::LITERAL:
      os << *s.sliteral << nl;
      break;
    case rpc_sym::NAMESPACE:
      break;
    default:
      std::cerr << "unknown type " << s.type << nl;
      break;
    }
    last_type = s.type;
  }

  os << endl << nl << "#endif /* !" << gtok << " */" << nl;
}

