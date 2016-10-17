// file      : build2/test/script/token.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/token>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      void
      token_printer (ostream& os, const token& t, bool d)
      {
        // Only quote non-name tokens for diagnostics.
        //
        const char* q (d ? "'" : "");

        switch (t.type)
        {
        case token_type::pipe:         os << q << '|' << q; break;
        case token_type::clean:        os << q << '&' << q; break;
        case token_type::log_and:      os << q << "&&" << q; break;
        case token_type::log_or:       os << q << "||" << q; break;

        case token_type::in_null:      os << q << "<!" << q; break;
        case token_type::in_string:    os << q << '<' << q; break;
        case token_type::in_document:  os << q << "<<" << q; break;

        case token_type::out_null:     os << q << ">!" << q; break;
        case token_type::out_string:   os << q << '>' << q; break;
        case token_type::out_document: os << q << ">>" << q; break;

        default: build2::token_printer (os, t, d);
        }
      }
    }
  }
}
