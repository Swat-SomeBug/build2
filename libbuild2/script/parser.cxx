// file      : libbuild2/script/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/parser.hxx>

#include <libbuild2/variable.hxx>
#include <libbuild2/script/run.hxx>   // exit
#include <libbuild2/script/lexer.hxx>

using namespace std;

namespace build2
{
  namespace script
  {
    using type = token_type;

    value parser::
    parse_variable_line (token& t, type& tt)
    {
      // enter: assignment
      // leave: newline or unknown token

      next_with_attributes (t, tt);

      // Parse value attributes if any. Note that it's ok not to have
      // anything after the attributes (e.g., foo=[null]).
      //
      attributes_push (t, tt, true);

      // @@ PAT: Should we expand patterns? Note that it will only be
      // simple ones since we have disabled {}. Also, what would be the
      // pattern base directory?
      //
      return tt != type::newline && start_names (tt)
        ? parse_value (t, tt,
                       pattern_mode::ignore,
                       "variable value",
                       nullptr)
        : value (names ());
    }

    // Parse the regular expression representation (non-empty string value
    // framed with introducer characters and optionally followed by flag
    // characters from the {di} set, for example '/foo/id') into
    // components. Also return end-of-parsing position if requested,
    // otherwise treat any unparsed characters left as an error.
    //
    struct regex_parts
    {
      string value;
      char   intro;
      string flags; // Combination of characters from {di} set.

      // Create a special empty object.
      //
      regex_parts (): intro ('\0') {}

      regex_parts (string v, char i, string f)
          : value (move (v)), intro (i), flags (move (f)) {}
    };

    static regex_parts
    parse_regex (const string& s,
                 const location& l,
                 const char* what,
                 size_t* end = nullptr)
    {
      if (s.empty ())
        fail (l) << "no introducer character in " << what;

      size_t p (s.find (s[0], 1)); // Find terminating introducer.

      if (p == string::npos)
        fail (l) << "no closing introducer character in " << what;

      size_t rn (p - 1); // Regex length.
      if (rn == 0)
        fail (l) << what << " is empty";

      // Find end-of-flags position.
      //
      size_t fp (++p); // Save flags starting position.
      for (char c; (c = s[p]) == 'd' || c == 'i'; ++p) ;

      // If string end is not reached then report invalid flags, unless
      // end-of-parsing position is requested (which means regex is just a
      // prefix).
      //
      if (s[p] != '\0' && end == nullptr)
        fail (l) << "junk at the end of " << what;

      if (end != nullptr)
        *end = p;

      return regex_parts (string (s, 1, rn), s[0], string (s, fp, p - fp));
    }

    optional<process_path> parser::
    parse_program (token& t, type& tt, names& ns)
    {
      parse_names (t, tt,
                   ns,
                   pattern_mode::ignore,
                   true /* chunk */,
                   "command line",
                   nullptr);

      return nullopt;
    }

    pair<command_expr, parser::here_docs> parser::
    parse_command_expr (token& t, type& tt,
                        const redirect_aliases& ra)
    {
      // enter: first token of the command line
      // leave: <newline> or unknown token

      command_expr expr;

      // OR-ed to an implied false for the first term.
      //
      expr.push_back ({expr_operator::log_or, command_pipe ()});

      command c; // Command being assembled.

      // Make sure the command makes sense.
      //
      auto check_command = [&c, this] (const location& l, bool last)
      {
        if (c.out && c.out->type == redirect_type::merge &&
            c.err && c.err->type == redirect_type::merge)
          fail (l) << "stdout and stderr redirected to each other";

        if (!last && c.out)
          fail (l) << "stdout is both redirected and piped";
      };

      // Check that the introducer character differs from '/' if the
      // portable path modifier is specified. Must be called before
      // parse_regex() (see below) to make sure its diagnostics is
      // meaningful.
      //
      // Note that the portable path modifier assumes '/' to be a valid
      // regex character and so makes it indistinguishable from the
      // terminating introducer.
      //
      auto check_regex_mod = [this] (const string& mod,
                                     const string& re,
                                     const location& l,
                                     const char* what)
      {
        // Handles empty regex properly.
        //
        if (mod.find ('/') != string::npos && re[0] == '/')
          fail (l) << "portable path modifier and '/' introducer in "
                   << what;
      };

      // Pending positions where the next word should go.
      //
      enum class pending
      {
        none,
        program,
        in_string,
        in_document,
        in_file,
        out_merge,
        out_string,
        out_str_regex,
        out_document,
        out_doc_regex,
        out_file,
        err_merge,
        err_string,
        err_str_regex,
        err_document,
        err_doc_regex,
        err_file,
        clean
      };
      pending p (pending::program);
      string mod;   // Modifiers for pending in_* and out_* positions.
      here_docs hd; // Expected here-documents.

      // Add the next word to either one of the pending positions or to
      // program arguments by default.
      //
      auto add_word = [&c, &p, &mod, &check_regex_mod, this] (
        string&& w, const location& l)
      {
        auto add_merge = [&l, this] (optional<redirect>& r,
                                     const string& w,
                                     int fd)
        {
          assert (r); // Must already be present.

          try
          {
            size_t n;
            if (stoi (w, &n) == fd && n == w.size ())
            {
              r->fd = fd;
              return;
            }
          }
          catch (const exception&) {} // Fall through.

          fail (l) << (fd == 1 ? "stderr" : "stdout") << " merge redirect "
                   << "file descriptor must be " << fd;
        };

        auto add_here_str = [] (optional<redirect>& r, string&& w)
        {
          assert (r); // Must already be present.

          if (r->modifiers ().find (':') == string::npos)
            w += '\n';
          r->str = move (w);
        };

        auto add_here_str_regex = [&l, &check_regex_mod] (
          optional<redirect>& r, int fd, string&& w)
        {
          assert (r); // Must already be present.

          const char* what (nullptr);
          switch (fd)
          {
          case 1: what = "stdout regex redirect"; break;
          case 2: what = "stderr regex redirect"; break;
          }

          check_regex_mod (r->modifiers (), w, l, what);

          regex_parts rp (parse_regex (w, l, what));

          regex_lines& re (r->regex);
          re.intro = rp.intro;

          re.lines.emplace_back (
            l.line, l.column, move (rp.value), move (rp.flags));

          // Add final blank line unless suppressed.
          //
          // Note that the position is synthetic, but that's ok as we don't
          // expect any diagnostics to refer this line.
          //
          if (r->modifiers ().find (':') == string::npos)
            re.lines.emplace_back (l.line, l.column, string (), false);
        };

        auto parse_path = [&l, this] (string&& w, const char* what) -> path
        {
          try
          {
            path p (move (w));

            if (!p.empty ())
            {
              p.normalize ();
              return p;
            }

            fail (l) << "empty " << what << endf;
          }
          catch (const invalid_path& e)
          {
            fail (l) << "invalid " << what << " '" << e.path << "'" << endf;
          }
        };

        auto add_file = [&parse_path] (optional<redirect>& r,
                                       int fd,
                                       string&& w)
        {
          assert (r); // Must already be present.

          const char* what (nullptr);
          switch (fd)
          {
          case 0: what = "stdin redirect path";  break;
          case 1: what = "stdout redirect path"; break;
          case 2: what = "stderr redirect path"; break;
          }

          r->file.path = parse_path (move (w), what);
        };

        switch (p)
        {
        case pending::none: c.arguments.push_back (move (w)); break;
        case pending::program:
          c.program = process_path (nullptr /* initial */,
                                    parse_path (move (w), "program path"),
                                    path () /* effect */);
          break;

        case pending::out_merge: add_merge (c.out, w, 2); break;
        case pending::err_merge: add_merge (c.err, w, 1); break;

        case pending::in_string:  add_here_str (c.in,  move (w)); break;
        case pending::out_string: add_here_str (c.out, move (w)); break;
        case pending::err_string: add_here_str (c.err, move (w)); break;

        case pending::out_str_regex:
          {
            add_here_str_regex (c.out, 1, move (w));
            break;
          }
        case pending::err_str_regex:
          {
            add_here_str_regex (c.err, 2, move (w));
            break;
          }

          // These are handled specially below.
          //
        case pending::in_document:
        case pending::out_document:
        case pending::err_document:
        case pending::out_doc_regex:
        case pending::err_doc_regex: assert (false); break;

        case pending::in_file:  add_file (c.in,  0, move (w)); break;
        case pending::out_file: add_file (c.out, 1, move (w)); break;
        case pending::err_file: add_file (c.err, 2, move (w)); break;

        case pending::clean:
          {
            cleanup_type t;
            switch (mod[0]) // Ok, if empty
            {
            case '!': t = cleanup_type::never;  break;
            case '?': t = cleanup_type::maybe;  break;
            default:  t = cleanup_type::always; break;
            }

            c.cleanups.push_back (
              {t, parse_path (move (w), "cleanup path")});
            break;
          }
        }

        p = pending::none;
        mod.clear ();
      };

      // Make sure we don't have any pending positions to fill.
      //
      auto check_pending = [&p, this] (const location& l)
      {
        const char* what (nullptr);

        switch (p)
        {
        case pending::none:                                            break;
        case pending::program:      what = "program";                  break;
        case pending::in_string:    what = "stdin here-string";        break;
        case pending::in_document:  what = "stdin here-document end";  break;
        case pending::in_file:      what = "stdin file";               break;
        case pending::out_merge:    what = "stdout file descriptor";   break;
        case pending::out_string:   what = "stdout here-string";       break;
        case pending::out_document: what = "stdout here-document end"; break;
        case pending::out_file:     what = "stdout file";              break;
        case pending::err_merge:    what = "stderr file descriptor";   break;
        case pending::err_string:   what = "stderr here-string";       break;
        case pending::err_document: what = "stderr here-document end"; break;
        case pending::err_file:     what = "stderr file";              break;
        case pending::clean:        what = "cleanup path";             break;

        case pending::out_str_regex:
          {
            what = "stdout here-string regex";
            break;
          }
        case pending::err_str_regex:
          {
            what = "stderr here-string regex";
            break;
          }
        case pending::out_doc_regex:
          {
            what = "stdout here-document regex end";
            break;
          }
        case pending::err_doc_regex:
          {
            what = "stderr here-document regex end";
            break;
          }
        }

        if (what != nullptr)
          fail (l) << "missing " << what;
      };

      // Parse the redirect operator.
      //
      // If the token type is the redirect alias then tt must contain the type
      // the alias resolves to and the token type otherwise. Note that this
      // argument defines the redirect semantics. Also note that the token is
      // saved into the redirect to keep the modifiers and the original
      // representation.
      //
      auto parse_redirect = [&c, &expr, &p, &mod, &hd, this]
                            (token&& t, type tt, const location& l)
      {
        // The redirect alias token type must be resolved.
        //
        assert (tt != type::in_l   &&
                tt != type::in_ll  &&
                tt != type::in_lll &&
                tt != type::out_g  &&
                tt != type::out_gg &&
                tt != type::out_ggg);

        // Our semantics is the last redirect seen takes effect.
        //
        assert (p == pending::none && mod.empty ());

        // See if we have the file descriptor.
        //
        unsigned long fd (3);
        if (!t.separated)
        {
          if (c.arguments.empty ())
            fail (l) << "missing redirect file descriptor";

          const string& s (c.arguments.back ());

          try
          {
            size_t n;
            fd = stoul (s, &n);

            if (n != s.size () || fd > 2)
              throw invalid_argument (string ());
          }
          catch (const exception&)
          {
            fail (l) << "invalid redirect file descriptor '" << s << "'";
          }

          c.arguments.pop_back ();
        }

        // Validate/set default file descriptor.
        //
        switch (tt)
        {
        case type::in_pass:
        case type::in_null:
        case type::in_str:
        case type::in_doc:
        case type::in_file:
          {
            if ((fd = fd == 3 ? 0 : fd) != 0)
              fail (l) << "invalid in redirect file descriptor " << fd;

            if (!expr.back ().pipe.empty ())
              fail (l) << "stdin is both piped and redirected";

            break;
          }
        case type::out_pass:
        case type::out_null:
        case type::out_trace:
        case type::out_merge:
        case type::out_str:
        case type::out_doc:
        case type::out_file_cmp:
        case type::out_file_ovr:
        case type::out_file_app:
          {
            if ((fd = fd == 3 ? 1 : fd) == 0)
              fail (l) << "invalid out redirect file descriptor " << fd;

            break;
          }
        }

        // Don't move as we will save the token into the redirect object.
        //
        mod = t.value;

        // Handle the none redirect (no data allowed) in the switch construct
        // if/when the respective syntax is invented.
        //
        redirect_type rt (redirect_type::none);
        switch (tt)
        {
        case type::in_pass:
        case type::out_pass:  rt = redirect_type::pass;  break;

        case type::in_null:
        case type::out_null:  rt = redirect_type::null;  break;

        case type::out_trace: rt = redirect_type::trace; break;

        case type::out_merge: rt = redirect_type::merge; break;

        case type::in_str:
        case type::out_str:
          {
            bool re (mod.find ('~') != string::npos);
            assert (tt == type::out_str || !re);

            rt = re
              ? redirect_type::here_str_regex
              : redirect_type::here_str_literal;

            break;
          }

        case type::in_doc:
        case type::out_doc:
          {
            bool re (mod.find ('~') != string::npos);
            assert (tt == type::out_doc || !re);

            rt = re
              ? redirect_type::here_doc_regex
              : redirect_type::here_doc_literal;

            break;
          }

        case type::in_file:
        case type::out_file_cmp:
        case type::out_file_ovr:
        case type::out_file_app: rt = redirect_type::file; break;
        }

        optional<redirect>& r (fd == 0 ? c.in  :
                               fd == 1 ? c.out :
                                         c.err);

        optional<redirect_type> overriden;

        if (r)
          overriden = r->type;

        r = redirect (rt);

        // Don't move as still may be used for pending here-document end
        // marker processing.
        //
        r->token = move (t);

        switch (rt)
        {
        case redirect_type::none:
          // Remove the assertion if/when the none redirect syntax is
          // invented.
          //
          assert (false);
          // Fall through.
        case redirect_type::pass:
        case redirect_type::null:
        case redirect_type::trace:
          break;
        case redirect_type::merge:
          switch (fd)
          {
          case 0: assert (false);         break;
          case 1: p = pending::out_merge; break;
          case 2: p = pending::err_merge; break;
          }
          break;
        case redirect_type::here_str_literal:
          switch (fd)
          {
          case 0: p = pending::in_string;  break;
          case 1: p = pending::out_string; break;
          case 2: p = pending::err_string; break;
          }
          break;
        case redirect_type::here_str_regex:
          switch (fd)
          {
          case 0: assert (false);             break;
          case 1: p = pending::out_str_regex; break;
          case 2: p = pending::err_str_regex; break;
          }
          break;
        case redirect_type::here_doc_literal:
          switch (fd)
          {
          case 0: p = pending::in_document;  break;
          case 1: p = pending::out_document; break;
          case 2: p = pending::err_document; break;
          }
          break;
        case redirect_type::here_doc_regex:
          switch (fd)
          {
          case 0: assert (false);             break;
          case 1: p = pending::out_doc_regex; break;
          case 2: p = pending::err_doc_regex; break;
          }
          break;
        case redirect_type::file:
          switch (fd)
          {
          case 0: p = pending::in_file;  break;
          case 1: p = pending::out_file; break;
          case 2: p = pending::err_file; break;
          }

          // Also sets for stdin, but this is harmless.
          //
          r->file.mode = tt == type::out_file_ovr ? redirect_fmode::overwrite :
                         tt == type::out_file_app ? redirect_fmode::append    :
                                                    redirect_fmode::compare;

          break;

        case redirect_type::here_doc_ref: assert (false); break;
        }

        // If we are overriding a here-document, then remove the reference
        // to this command redirect from the corresponding here_doc object.
        //
        if (!pre_parse_ &&
            overriden &&
            (*overriden == redirect_type::here_doc_literal ||
             *overriden == redirect_type::here_doc_regex))
        {
          size_t e (expr.size () - 1);
          size_t p (expr.back ().pipe.size ());
          int    f (static_cast<int> (fd));

          for (here_doc& d: hd)
          {
            small_vector<here_redirect, 2>& rs (d.redirects);

            auto i (find_if (rs.begin (), rs.end (),
                             [e, p, f] (const here_redirect& r)
                             {
                               return r.expr == e &&
                                 r.pipe == p &&
                                 r.fd   == f;
                             }));

            if (i != rs.end ())
            {
              rs.erase (i);
              break;
            }
          }
        }
      };

      // Set pending cleanup type.
      //
      auto parse_clean = [&p, &mod] (token& t)
      {
        p = pending::clean;
        mod = move (t.value);
      };

      const location ll (get_location (t)); // Line location.

      // Keep parsing chunks of the command line until we see one of the
      // "terminators" (newline, exit status comparison, etc).
      //
      location l (ll);
      names ns; // Reuse to reduce allocations.

      for (bool done (false); !done; l = get_location (t))
      {
        tt = ra.resolve (tt);

        switch (tt)
        {
        case type::newline:
          {
            done = true;
            break;
          }

        case type::equal:
        case type::not_equal:
          {
            if (!pre_parse_)
              check_pending (l);

            c.exit = parse_command_exit (t, tt);

            // Only a limited set of things can appear after the exit status
            // so we check this here.
            //
            switch (tt)
            {
            case type::newline:

            case type::pipe:
            case type::log_or:
            case type::log_and:
              break;

            default:
              {
                // Bail out if this is one of the unknown/unexpected tokens.
                //
                done = true;
                break;
              }
            }

            break;
          }

        case type::pipe:
        case type::log_or:
        case type::log_and:

        case type::in_pass:
        case type::out_pass:

        case type::in_null:
        case type::out_null:

        case type::out_trace:

        case type::out_merge:

        case type::in_str:
        case type::in_doc:
        case type::out_str:
        case type::out_doc:

        case type::in_file:
        case type::out_file_cmp:
        case type::out_file_ovr:
        case type::out_file_app:

        case type::clean:
          {
            if (pre_parse_)
            {
              // The only things we need to handle here are the tokens that
              // introduce the next command, since we handle the command
              // leading name chunks specially, and the here-document and
              // here-document regex end markers, since we need to know how
              // many of them to pre-parse after the command.
              //
              switch (tt)
              {
              case type::pipe:
              case type::log_or:
              case type::log_and:
                p = pending::program;
                break;

              case type::in_doc:
              case type::out_doc:
                mod = move (t.value);

                bool re (mod.find ('~') != string::npos);
                const char* what (re
                                  ? "here-document regex end marker"
                                  : "here-document end marker");

                // We require the end marker to be a literal, unquoted word.
                // In particularm, we don't allow quoted because of cases
                // like foo"$bar" (where we will see word 'foo').
                //
                next (t, tt);

                // We require the end marker to be an unquoted or completely
                // quoted word. The complete quoting becomes important for
                // cases like foo"$bar" (where we will see word 'foo').
                //
                // For good measure we could have also required it to be
                // separated from the following token, but out grammar
                // allows one to write >>EOO;. The problematic sequence
                // would be >>FOO$bar -- on reparse it will be expanded
                // as a single word.
                //
                if (tt != type::word || t.value.empty ())
                  fail (t) << "expected " << what;

                peek ();
                const token& p (peeked ());
                if (!p.separated)
                {
                  switch (p.type)
                  {
                  case type::dollar:
                  case type::lparen:
                    fail (p) << what << " must be literal";
                  }
                }

                quote_type qt (t.qtype);
                switch (qt)
                {
                case quote_type::unquoted:
                  qt = quote_type::single; // Treat as single-quoted.
                  break;
                case quote_type::single:
                case quote_type::double_:
                  if (t.qcomp)
                    break;
                  // Fall through.
                case quote_type::mixed:
                  fail (t) << "partially-quoted " << what;
                }

                regex_parts r;
                string end (move (t.value));

                if (re)
                {
                  check_regex_mod (mod, end, l, what);

                  r = parse_regex (end, l, what);
                  end = move (r.value); // The "cleared" end marker.
                }

                bool literal (qt == quote_type::single);
                bool shared (false);

                for (const auto& d: hd)
                {
                  if (d.end == end)
                  {
                    auto check = [&t, &end, &re, this] (bool c,
                                                        const char* what)
                    {
                      if (!c)
                        fail (t) << "different " << what
                                 << " for shared here-document "
                                 << (re ? "regex '" : "'") << end << "'";
                    };

                    check (d.modifiers == mod, "modifiers");
                    check (d.literal == literal, "quoting");

                    if (re)
                    {
                      check (d.regex == r.intro, "introducers");
                      check (d.regex_flags == r.flags, "global flags");
                    }

                    shared = true;
                    break;
                  }
                }

                if (!shared)
                  hd.push_back (
                    here_doc {
                      {},
                      move (end),
                      literal,
                      move (mod),
                      r.intro, move (r.flags)});

                break;
              }

              next (t, tt);
              break;
            }

            // If this is one of the operators/separators, check that we
            // don't have any pending locations to be filled.
            //
            check_pending (l);

            // Note: there is another one in the inner loop below.
            //
            switch (tt)
            {
            case type::pipe:
            case type::log_or:
            case type::log_and:
              {
                // Check that the previous command makes sense.
                //
                check_command (l, tt != type::pipe);
                expr.back ().pipe.push_back (move (c));

                c = command ();
                p = pending::program;

                if (tt != type::pipe)
                {
                  expr_operator o (tt == type::log_or
                                   ? expr_operator::log_or
                                   : expr_operator::log_and);
                  expr.push_back ({o, command_pipe ()});
                }

                break;
              }

            case type::in_pass:
            case type::out_pass:

            case type::in_null:
            case type::out_null:

            case type::out_trace:

            case type::out_merge:

            case type::in_str:
            case type::in_doc:
            case type::out_str:
            case type::out_doc:

            case type::in_file:
            case type::out_file_cmp:
            case type::out_file_ovr:
            case type::out_file_app:
              {
                parse_redirect (move (t), tt, l);
                break;
              }

            case type::clean:
              {
                parse_clean (t);
                break;
              }

            default: assert (false); break;
            }

            next (t, tt);
            break;
          }
        default:
          {
            // Bail out if this is one of the unknown tokens.
            //
            if (!start_names (tt))
            {
              done = true;
              break;
            }

            // Here-document end markers are literal (we verified that above
            // during pre-parsing) and we need to know whether they were
            // quoted. So handle this case specially.
            //
            {
              int fd;
              switch (p)
              {
              case pending::in_document:   fd =  0; break;
              case pending::out_document:
              case pending::out_doc_regex: fd =  1; break;
              case pending::err_document:
              case pending::err_doc_regex: fd =  2; break;
              default:                     fd = -1; break;
              }

              if (fd != -1)
              {
                if (tt != type::word || t.value.empty ())
                  fail (t) << "expected here-document end marker";

                here_redirect rd {
                  expr.size () - 1, expr.back ().pipe.size (), fd};

                string end (move (t.value));

                regex_parts r;

                if (p == pending::out_doc_regex ||
                    p == pending::err_doc_regex)
                {
                  // We can't fail here as we already parsed all the end
                  // markers during pre-parsing stage, and so no need in the
                  // description.
                  //
                  r = parse_regex (end, l, "");
                  end = move (r.value); // The "cleared" end marker.
                }

                bool shared (false);
                for (auto& d: hd)
                {
                  // No need to check that redirects that share here-document
                  // have the same modifiers, etc. That have been done during
                  // pre-parsing.
                  //
                  if (d.end == end)
                  {
                    d.redirects.emplace_back (rd);
                    shared = true;
                    break;
                  }
                }

                if (!shared)
                  hd.push_back (
                    here_doc {
                      {rd},
                        move (end),
                          (t.qtype == quote_type::unquoted ||
                           t.qtype == quote_type::single),
                          move (mod),
                          r.intro, move (r.flags)});

                p = pending::none;
                mod.clear ();

                next (t, tt);
                break;
              }
            }

            // Parse the next chunk as names to get expansion, etc. Note that
            // we do it in the chunking mode to detect whether anything in
            // each chunk is quoted. If we are waiting for the command
            // program, then delegate the parsing to the derived parser, so it
            // can translate complex program names (targets, process_paths)
            // during execution and perform some static analysis during
            // pre-parsing.
            //
            // @@ PAT: should we support pattern expansion? This is even
            // fuzzier than the variable case above. Though this is the
            // shell semantics. Think what happens when we do rm *.txt?
            //
            reset_quoted (t);

            if (p == pending::program)
            {
              optional<process_path> pp (parse_program (t, tt, ns));

              // During pre-parsing we are not interested in the
              // parse_program() call result, so just discard the potentially
              // unhandled program chunk names.
              //
              if (!pre_parse_)
              {
                if (pp)
                {
                  c.program = move (*pp);
                  p = pending::none;
                }
              }
              else
              {
                ns.clear ();
                p = pending::none;
              }
            }
            else
              parse_names (t, tt,
                           ns,
                           pattern_mode::ignore,
                           true /* chunk */,
                           "command line",
                           nullptr);

            // Nothing else to do if we are pre-parsing.
            //
            if (pre_parse_)
              break;

            // Process what we got. Determine whether anything inside was
            // quoted (note that the current token is "next" and is not part
            // of this).
            //
            bool q ((quoted () -
                     (t.qtype != quote_type::unquoted ? 1 : 0)) != 0);

            for (name& n: ns)
            {
              string s;

              try
              {
                s = value_traits<string>::convert (move (n), nullptr);
              }
              catch (const invalid_argument&)
              {
                diag_record dr (fail (l));
                dr << "invalid string value ";
                to_stream (dr.os, n, true); // Quote.
              }

              // If it is a quoted chunk, then we add the word as is.
              // Otherwise we re-lex it. But if the word doesn't contain any
              // interesting characters (operators plus quotes/escapes),
              // then no need to re-lex.
              //
              // NOTE: update quoting (script.cxx:to_stream_q()) if adding
              // any new characters.
              //
              if (q || s.find_first_of ("|&<>\'\"\\") == string::npos)
                add_word (move (s), l);
              else
              {
                // If the chunk re-parsing results in error, our diagnostics
                // will look like this:
                //
                // <string>:1:4: error: stdout merge redirect file descriptor must be 2
                //   script:2:5: info: while parsing string '1>&a'
                //
                auto df = make_diag_frame (
                  [this, s, &l](const diag_record& dr)
                  {
                    dr << info (l) << "while parsing string '" << s << "'";
                  });

                // When re-lexing we do "effective escaping" and only for
                // ['"\] (quotes plus the backslash itself). In particular,
                // there is no way to escape redirects, operators, etc. The
                // idea is to prefer quoting except for passing literal
                // quotes, for example:
                //
                // args = \"&foo\"
                // cmd $args               # cmd &foo
                //
                // args = 'x=\"foo bar\"'
                // cmd $args               # cmd x="foo bar"
                //
                istringstream is (s);
                path_name in ("<string>");
                lexer lex (is, in,
                           lexer_mode::command_expansion,
                           ra,
                           "\'\"\\");

                // Treat the first "sub-token" as always separated from what
                // we saw earlier.
                //
                // Note that this is not "our" token so we cannot do
                // fail(t). Rather we should do fail(l).
                //
                token t (lex.next ());
                location l (build2::get_location (t, in));
                t.separated = true;

                string w;
                bool f (t.type == type::eos); // If the whole thing is empty.

                for (; t.type != type::eos; t = lex.next ())
                {
                  type tt (ra.resolve (t.type));
                  l = build2::get_location (t, in);

                  // Re-lexing double-quotes will recognize $, ( inside as
                  // tokens so we have to reverse them back. Since we don't
                  // treat spaces as separators we can be sure we will get
                  // it right.
                  //
                  switch (tt)
                  {
                  case type::dollar: w += '$'; continue;
                  case type::lparen: w += '('; continue;
                  }

                  // Retire the current word. We need to distinguish between
                  // empty and non-existent (e.g., > vs >"").
                  //
                  if (!w.empty () || f)
                  {
                    add_word (move (w), l);
                    f = false;
                  }

                  if (tt == type::word)
                  {
                    w = move (t.value);
                    f = true;
                    continue;
                  }

                  // If this is one of the operators/separators, check that
                  // we don't have any pending locations to be filled.
                  //
                  check_pending (l);

                  // Note: there is another one in the outer loop above.
                  //
                  switch (tt)
                  {
                  case type::pipe:
                  case type::log_or:
                  case type::log_and:
                    {
                      // Check that the previous command makes sense.
                      //
                      check_command (l, tt != type::pipe);
                      expr.back ().pipe.push_back (move (c));

                      c = command ();
                      p = pending::program;

                      if (tt != type::pipe)
                      {
                        expr_operator o (tt == type::log_or
                                         ? expr_operator::log_or
                                         : expr_operator::log_and);
                        expr.push_back ({o, command_pipe ()});
                      }

                      break;
                    }

                  case type::in_pass:
                  case type::out_pass:

                  case type::in_null:
                  case type::out_null:

                  case type::out_trace:

                  case type::out_merge:

                  case type::in_str:
                  case type::out_str:

                  case type::in_file:
                  case type::out_file_cmp:
                  case type::out_file_ovr:
                  case type::out_file_app:
                    {
                      parse_redirect (move (t), tt, l);
                      break;
                    }

                  case type::clean:
                    {
                      parse_clean (t);
                      break;
                    }

                  case type::in_doc:
                  case type::out_doc:
                    {
                      fail (l) << "here-document redirect in expansion";
                      break;
                    }
                  }
                }

                // Don't forget the last word.
                //
                if (!w.empty () || f)
                  add_word (move (w), l);
              }
            }

            ns.clear ();
            break;
          }
        }
      }

      if (!pre_parse_)
      {
        // Verify we don't have anything pending to be filled and the
        // command makes sense.
        //
        check_pending (l);
        check_command (l, true);

        expr.back ().pipe.push_back (move (c));
      }

      return make_pair (move (expr), move (hd));
    }

    command_exit parser::
    parse_command_exit (token& t, type& tt)
    {
      // enter: equal/not_equal
      // leave: token after exit status (one parse_names() chunk)

      exit_comparison comp (tt == type::equal
                            ? exit_comparison::eq
                            : exit_comparison::ne);

      // The next chunk should be the exit status.
      //
      next (t, tt);
      location l (get_location (t));
      names ns (parse_names (t, tt,
                             pattern_mode::ignore,
                             true,
                             "exit status",
                             nullptr));
      unsigned long es (256);

      if (!pre_parse_)
      {
        try
        {
          if (ns.size () == 1 && ns[0].simple () && !ns[0].empty ())
            es = stoul (ns[0].value);
        }
        catch (const exception&) {} // Fall through.

        if (es > 255)
        {
          diag_record dr;

          dr << fail (l) << "expected exit status instead of ";
          to_stream (dr.os, ns, true); // Quote.

          dr << info << "exit status is an unsigned integer less than 256";
        }
      }

      return command_exit {comp, static_cast<uint8_t> (es)};
    }

    void parser::
    parse_here_documents (token& t, type& tt,
                          pair<command_expr, here_docs>& p)
    {
      // enter: newline
      // leave: newline

      // Parse here-document fragments in the order they were mentioned on
      // the command line.
      //
      for (here_doc& h: p.second)
      {
        // Switch to the here-line mode which is like single/double-quoted
        // string but recognized the newline as a separator.
        //
        mode (h.literal
              ? lexer_mode::here_line_single
              : lexer_mode::here_line_double);
        next (t, tt);

        parsed_doc v (
          parse_here_document (t, tt, h.end, h.modifiers, h.regex));

        // If all the here-document redirects are overridden, then we just
        // drop the fragment.
        //
        if (!pre_parse_ && !h.redirects.empty ())
        {
          auto i (h.redirects.cbegin ());

          command& c (p.first[i->expr].pipe[i->pipe]);

          optional<redirect>& r (i->fd == 0 ? c.in  :
                                 i->fd == 1 ? c.out :
                                              c.err);

          assert (r); // Must be present since it is referred.

          if (v.re)
          {
            assert (r->type == redirect_type::here_doc_regex);

            r->regex = move (v.regex);
            r->regex.flags = move (h.regex_flags);
          }
          else
          {
            assert (r->type == redirect_type::here_doc_literal);

            r->str = move (v.str);
          }

          r->end        = move (h.end);
          r->end_line   = v.end_line;
          r->end_column = v.end_column;

          // Note that our references cannot be invalidated because the
          // command_expr/command-pipe vectors already contain all their
          // elements.
          //
          for (++i; i != h.redirects.cend (); ++i)
          {
            command& c (p.first[i->expr].pipe[i->pipe]);

            optional<redirect>& ir (i->fd == 0 ? c.in  :
                                    i->fd == 1 ? c.out :
                                                 c.err);

            // Must be present since it is referenced by here-doc.
            //
            assert (ir);

            // Note: preserve the original representation.
            //
            ir = redirect (redirect_type::here_doc_ref, *r, move (ir->token));
          }
        }

        expire_mode ();
      }
    }

    parser::parsed_doc parser::
    parse_here_document (token& t, type& tt,
                         const string& em,
                         const string& mod,
                         char re)
    {
      // enter: first token on first line
      // leave: newline (after end marker)

      // String literal. Note that when decide if to terminate the previously
      // added line with a newline, we need to distinguish a yet empty result
      // and the one that has a single blank line added.
      //
      optional<string> rs;

      regex_lines rre;

      // Here-documents can be indented. The leading whitespaces of the end
      // marker line (called strip prefix) determine the indentation. Every
      // other line in the here-document should start with this prefix which
      // is automatically stripped. The only exception is a blank line.
      //
      // The fact that the strip prefix is only known at the end, after
      // seeing all the lines, is rather inconvenient. As a result, the way
      // we implement this is a bit hackish (though there is also something
      // elegant about it): at the end of the pre-parse stage we are going
      // re-examine the sequence of tokens that comprise this here-document
      // and "fix up" the first token of each line by stripping the prefix.
      //
      string sp;

      // Remember the position of the first token in this here-document.
      //
      size_t ri (pre_parse_ ? replay_data_.size () - 1 : 0);

      // We will use the location of the first token on the line for the
      // regex diagnostics. At the end of the loop it will point to the
      // beginning of the end marker.
      //
      location l;

      while (tt != type::eos)
      {
        l = get_location (t);

        // Check if this is the end marker. For starters, it should be a
        // single, unquoted word followed by a newline.
        //
        if (tt == type::word &&
            t.qtype == quote_type::unquoted &&
            peek () == type::newline)
        {
          const string& v (t.value);

          size_t vn (v.size ());
          size_t en (em.size ());

          // Then check that it ends with the end marker.
          //
          if (vn >= en && v.compare (vn - en, en, em) == 0)
          {
            // Now check that the prefix only contains whitespaces.
            //
            size_t n (vn - en);

            if (v.find_first_not_of (" \t") >= n)
            {
              assert (pre_parse_ || n == 0); // Should have been stripped.

              if (n != 0)
                sp.assign (v, 0, n); // Save the strip prefix.

              next (t, tt); // Get the newline.
              break;
            }
          }
        }

        // Expand the line (can be blank).
        //
        // @@ PAT: one could argue that if we do it in variables, then we
        // should do it here as well. Though feels bizarre.
        //
        names ns (tt != type::newline
                  ? parse_names (t, tt,
                                 pattern_mode::ignore,
                                 false,
                                 "here-document line",
                                 nullptr)
                  : names ());

        if (!pre_parse_)
        {
          // What shall we do if the expansion results in multiple names?
          // For, example if the line contains just the variable expansion
          // and it is of type strings. Adding all the elements space-
          // separated seems like the natural thing to do.
          //
          string s;
          for (auto b (ns.begin ()), i (b); i != ns.end (); ++i)
          {
            string n;

            try
            {
              n = value_traits<string>::convert (move (*i), nullptr);
            }
            catch (const invalid_argument&)
            {
              fail (l) << "invalid string value '" << *i << "'";
            }

            if (i == b)
              s = move (n);
            else
            {
              s += ' ';
              s += n;
            }
          }

          if (!re)
          {
            // Add newline after previous line.
            //
            if (rs)
            {
              *rs += '\n';
              *rs += s;
            }
            else
              rs = move (s);
          }
          else
          {
            // Due to expansion we can end up with multiple lines. If empty
            // then will add a blank textual literal.
            //
            for (size_t p (0); p != string::npos; )
            {
              string ln;
              size_t np (s.find ('\n', p));

              if (np != string::npos)
              {
                ln = string (s, p, np - p);
                p = np + 1;
              }
              else
              {
                ln = string (s, p);
                p = np;
              }

              if (ln[0] != re) // Line doesn't start with regex introducer.
              {
                // This is a line-char literal (covers blank lines as well).
                //
                // Append textual literal.
                //
                rre.lines.emplace_back (l.line, l.column, move (ln), false);
              }
              else // Line starts with the regex introducer.
              {
                // This is a char-regex, or a sequence of line-regex syntax
                // characters or both (in this specific order). So we will
                // add regex (with optional special characters) or special
                // literal.
                //
                size_t p (ln.find (re, 1));
                if (p == string::npos)
                {
                  // No regex, just a sequence of syntax characters.
                  //
                  string spec (ln, 1);
                  if (spec.empty ())
                    fail (l) << "no syntax line characters";

                  // Append special literal.
                  //
                  rre.lines.emplace_back (
                    l.line, l.column, move (spec), true);
                }
                else
                {
                  // Regex (probably with syntax characters).
                  //
                  regex_parts re;

                  // Empty regex is a special case repesenting a blank line.
                  //
                  if (p == 1)
                    // Position to optional specal characters of an empty
                    // regex.
                    //
                    ++p;
                  else
                    // Can't fail as all the pre-conditions verified
                    // (non-empty with both introducers in place), so no
                    // description required.
                    //
                    re = parse_regex (ln, l, "", &p);

                  // Append regex with optional special characters.
                  //
                  rre.lines.emplace_back (l.line, l.column,
                                          move (re.value), move (re.flags),
                                          string (ln, p));
                }
              }
            }
          }
        }

        // We should expand the whole line at once so this would normally be
        // a newline but can also be an end-of-stream.
        //
        if (tt == type::newline)
          next (t, tt);
        else
          assert (tt == type::eos);
      }

      if (tt == type::eos)
        fail (t) << "missing here-document end marker '" << em << "'";

      if (pre_parse_)
      {
        // Strip the indentation prefix if there is one.
        //
        assert (replay_ == replay::save);

        if (!sp.empty ())
        {
          size_t sn (sp.size ());

          for (; ri != replay_data_.size (); ++ri)
          {
            token& rt (replay_data_[ri].token);

            if (rt.type == type::newline) // Blank
              continue;

            if (rt.type != type::word || rt.value.compare (0, sn, sp) != 0)
              fail (rt) << "unindented here-document line";

            // If the word is equal to the strip prefix then we have to drop
            // the token. Note that simply making it an empty word won't
            // have the same semantics. For instance, it would trigger
            // concatenated expansion.
            //
            if (rt.value.size () == sn)
              replay_data_.erase (replay_data_.begin () + ri);
            else
            {
              rt.value.erase (0, sn);
              rt.column += sn;
              ++ri;
            }

            // Skip until next newline.
            //
            for (; replay_data_[ri].token.type != type::newline; ++ri) ;
          }
        }
      }
      else
      {
        // Add final newline unless suppressed.
        //
        if (mod.find (':') == string::npos)
        {
          if (re)
            // Note that the position is synthetic, but that's ok as we don't
            // expect any diagnostics to refer this line.
            //
            rre.lines.emplace_back (l.line, l.column, string (), false);
          else if (rs)
            *rs += '\n';
          else
            rs = "\n";
        }

        // Finalize regex lines.
        //
        if (re)
        {
          // Empty regex matches nothing, so not of much use.
          //
          if (rre.lines.empty ())
            fail (l) << "empty here-document regex";

          rre.intro  = re;
        }
      }

      return re
        ? parsed_doc (move (rre), l.line, l.column)
        : parsed_doc (rs ? move (*rs) : string (), l.line, l.column);
    }

    size_t parser::
    quoted () const
    {
      size_t r (0);

      if (replay_ != replay::play)
        r = lexer_->quoted ();
      else
      {
        // Examine tokens we have replayed since last reset.
        //
        for (size_t i (replay_quoted_); i != replay_i_; ++i)
          if (replay_data_[i].token.qtype != quote_type::unquoted)
            ++r;
      }

      return r;
    }

    void parser::
    reset_quoted (token& cur)
    {
      if (replay_ != replay::play)
        lexer_->reset_quoted (cur.qtype != quote_type::unquoted ? 1 : 0);
      else
      {
        replay_quoted_ = replay_i_ - 1;

        // Must be the same token.
        //
        assert (replay_data_[replay_quoted_].token.qtype == cur.qtype);
      }
    }

    void parser::
    set_lexer (lexer* l)
    {
      lexer_ = l;
      build2::parser::lexer_ = l;
    }

    static redirect_aliases no_redirect_aliases;

    void parser::
    apply_value_attributes (const variable* var,
                            value& lhs,
                            value&& rhs,
                            const string& attributes,
                            token_type kind,
                            const path_name& name)
    {
      path_ = &name;

      istringstream is (attributes);

      // Note that the redirect alias information is not used in the
      // attributes lexer mode.
      //
      lexer l (is, name, lexer_mode::attributes, no_redirect_aliases);

      set_lexer (&l);

      token t;
      type tt;

      next_with_attributes (t, tt); // Enable `[` recognition.

      if (tt != type::lsbrace && tt != type::eos)
        fail (t) << "expected '[' instead of " << t;

      attributes_push (t, tt, true);

      if (tt != type::eos)
        fail (t) << "trailing junk after ']'";

      build2::parser::apply_value_attributes (var, lhs, move (rhs), kind);
    }

    line_type parser::
    pre_parse_line_start (token& t, token_type& tt, lexer_mode stm)
    {
      replay_save (); // Start saving tokens from the current one.
      next (t, tt);

      // Decide whether this is a variable assignment or a command.
      //
      // It is an assignment if the first token is an unquoted name and
      // the next token is an assign/append/prepend operator. Assignment
      // to a computed variable name must use the set builtin.
      //
      // Note also that special commands take precedence over variable
      // assignments.
      //
      line_type r (line_type::cmd); // Default.

      if (tt == type::word && t.qtype == quote_type::unquoted)
      {
        const string& n (t.value);

        if      (n == "if")    r = line_type::cmd_if;
        else if (n == "if!")   r = line_type::cmd_ifn;
        else if (n == "elif")  r = line_type::cmd_elif;
        else if (n == "elif!") r = line_type::cmd_elifn;
        else if (n == "else")  r = line_type::cmd_else;
        else if (n == "end")   r = line_type::cmd_end;
        else
        {
          // Switch the recognition of leading variable assignments for
          // the next token. This is safe to do because we know we
          // cannot be in the quoted mode (since the current token is
          // not quoted).
          //
          type p (peek (stm));

          if (p == type::assign  || p == type::prepend || p == type::append)
          {
            r = line_type::var;

            // Note that the missing command program is detected later, by
            // parse_command_expr().
            //
            if (n.empty ())
              fail (t) << "missing variable name";
          }
        }
      }

      return r;
    }

    bool parser::
    exec_lines (lines::const_iterator i, lines::const_iterator e,
                const function<exec_set_function>& exec_set,
                const function<exec_cmd_function>& exec_cmd,
                const function<exec_if_function>& exec_if,
                size_t& li,
                variable_pool* var_pool)
    {
      try
      {
        token t;
        type tt;
        for (; i != e; ++i)
        {
          const line& ln (*i);
          line_type lt (ln.type);

          assert (path_ == nullptr);

          // Copy the tokens and start playing.
          //
          replay_data (replay_tokens (ln.tokens));

          // We don't really need to change the mode since we already know
          // the line type.
          //
          next (t, tt);
          const location ll (get_location (t));

          switch (lt)
          {
          case line_type::var:
            {
              // Enter the variable into the pool if this is not done during
              // the script parsing. Note that in this case the pool is
              // expected to be provided.
              //
              const variable* var (ln.var);

              if (var == nullptr)
              {
                assert (var_pool != nullptr);

                var = &var_pool->insert (t.value);
              }

              exec_set (*var, t, tt, ll);

              replay_stop ();
              break;
            }
          case line_type::cmd:
            {
              bool single (false);

              if (li == 1)
              {
                lines::const_iterator j (i);
                for (++j; j != e && j->type == line_type::var; ++j) ;

                if (j == e) // We have no another command.
                  single = true;
              }

              exec_cmd (t, tt, li++, single, ll);

              replay_stop ();
              break;
            }
          case line_type::cmd_if:
          case line_type::cmd_ifn:
          case line_type::cmd_elif:
          case line_type::cmd_elifn:
          case line_type::cmd_else:
            {
              next (t, tt); // Skip to start of command.

              bool take;
              if (lt != line_type::cmd_else)
              {
                take = exec_if (t, tt, li++, ll);

                if (lt == line_type::cmd_ifn || lt == line_type::cmd_elifn)
                  take = !take;
              }
              else
              {
                assert (tt == type::newline);
                take = true;
              }

              replay_stop ();

              // If end is true, then find the 'end' line. Otherwise, find
              // the next if-else line. If skip is true then increment the
              // command line index.
              //
              auto next = [e, &li] (lines::const_iterator j,
                                    bool end,
                                    bool skip) -> lines::const_iterator
                {
                  // We need to be aware of nested if-else chains.
                  //
                  size_t n (0);

                  for (++j; j != e; ++j)
                  {
                    line_type lt (j->type);

                    if (lt == line_type::cmd_if || lt == line_type::cmd_ifn)
                      ++n;

                    // If we are nested then we just wait until we get back
                    // to the surface.
                    //
                    if (n == 0)
                    {
                      switch (lt)
                      {
                      case line_type::cmd_elif:
                      case line_type::cmd_elifn:
                      case line_type::cmd_else:
                        if (end) break;
                        // Fall through.
                      case line_type::cmd_end:  return j;
                      default: break;
                      }
                    }

                    if (lt == line_type::cmd_end)
                      --n;

                    if (skip)
                    {
                      // Note that we don't count else and end as commands.
                      //
                      switch (lt)
                      {
                      case line_type::cmd:
                      case line_type::cmd_if:
                      case line_type::cmd_ifn:
                      case line_type::cmd_elif:
                      case line_type::cmd_elifn: ++li; break;
                      default:                         break;
                      }
                    }
                  }

                  assert (false); // Missing end.
                  return e;
                };

              // If we are taking this branch then we need to parse all the
              // lines until the next if-else line and then skip all the
              // lines until the end (unless next is already end).
              //
              // Otherwise, we need to skip all the lines until the next
              // if-else line and then continue parsing.
              //
              if (take)
              {
                // Next if-else.
                //
                lines::const_iterator j (next (i, false, false));
                if (!exec_lines (i + 1, j,
                                 exec_set, exec_cmd, exec_if,
                                 li,
                                 var_pool))
                  return false;

                i = j->type == line_type::cmd_end ? j : next (j, true, true);
              }
              else
              {
                i = next (i, false, true);
                if (i->type != line_type::cmd_end)
                  --i; // Continue with this line (e.g., elif or else).
              }

              break;
            }
          case line_type::cmd_end:
            {
              assert (false);
            }
          }
        }

        return true;
      }
      catch (const exit& e)
      {
        // Bail out if the script is exited with the failure status. Otherwise
        // exit the lines execution normally.
        //
        if (!e.status)
          throw failed ();

        replay_stop ();
        return false;
      }
    }

    // parser::parsed_doc
    //
    parser::parsed_doc::
    parsed_doc (string s, uint64_t l, uint64_t c)
        : str (move (s)), re (false), end_line (l), end_column (c)
    {
    }

    parser::parsed_doc::
    parsed_doc (regex_lines&& r, uint64_t l, uint64_t c)
        : regex (move (r)), re (true), end_line (l), end_column (c)
    {
    }

    parser::parsed_doc::
    parsed_doc (parsed_doc&& d)
        : re (d.re), end_line (d.end_line), end_column (d.end_column)
    {
      if (re)
        new (&regex) regex_lines (move (d.regex));
      else
        new (&str) string (move (d.str));
    }

    parser::parsed_doc::
    ~parsed_doc ()
    {
      if (re)
        regex.~regex_lines ();
      else
        str.~string ();
    }
  }
}
