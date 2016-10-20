// file      : build2/test/script/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/parser>

#include <build2/test/script/lexer>
#include <build2/test/script/runner>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      void parser::
      pre_parse (istream& is, const path& p, script& s)
      {
        path_ = &p;

        lexer l (is, *path_, lexer_mode::script_line);
        lexer_ = &l;
        base_parser::lexer_ = &l;

        script_ = &s;
        runner_ = nullptr;
        scope_ = script_;

        pre_parse_ = true;

        pre_parse_script ();
      }

      void parser::
      parse (const path& p, script& s, runner& r)
      {
        path_ = &p;

        lexer_ = nullptr;
        base_parser::lexer_ = nullptr;

        script_ = &s;
        runner_ = &r;
        scope_ = script_;

        pre_parse_ = false;

        parse_script ();
      }

      void parser::
      pre_parse_script ()
      {
        token t;
        type tt;

        for (;;)
        {
          // Start saving tokens for the next (logical) line.
          //
          replay_save ();

          // We need to start lexing each line in the assign mode in order to
          // recognize assignment operators as separators.
          //
          mode (lexer_mode::assign_line);
          next (t, tt);

          if (tt == type::eos)
            break;

          line_type lt (pre_parse_script_line (t, tt));
          assert (tt == type::newline);

          // Stop saving and get the tokens.
          //
          scope_->lines.push_back (line {lt, replay_data ()});
        }

        replay_stop (); // Discard replay of eos.
      }

      void parser::
      parse_script ()
      {
        token t;
        type tt;

        for (line& l: scope_->lines)
        {
          replay_data (move (l.tokens)); // Set the tokens and start playing.

          // We don't really need the assign mode since we already know the
          // line type.
          //
          next (t, tt);

          parse_script_line (t, tt, l.type);
          assert (tt == type::newline);

          replay_stop (); // Stop playing.
        }
      }

      line_type parser::
      pre_parse_script_line (token& t, type& tt)
      {
        // Decide whether this is a variable assignment or a command. It is a
        // variable assignment if the first token is an unquoted word (name)
        // and the next is an assign/append/prepend operator. Assignment to a
        // computed variable name must use the set builtin.
        //
        if (tt == type::word && !t.quoted)
        {
          // Switch recognition of variable assignments for one more token.
          // This is safe to do because we know we cannot be in the quoted
          // mode (since the current token is not quoted).
          //
          mode (lexer_mode::assign_line);
          type p (peek ());

          if (p == type::assign || p == type::prepend || p == type::append)
          {
            parse_variable_line (t, tt);
            return line_type::variable;
          }
        }

        parse_test_line (t, tt);
        return line_type::test;
      }

      void parser::
      parse_script_line (token& t, type& tt, line_type lt)
      {
        switch (lt)
        {
        case line_type::variable: parse_variable_line (t, tt); break;
        case line_type::test:     parse_test_line     (t, tt); break;
        }
      }

      // Return true if the string contains only digit characters (used to
      // detect the special $NN variables).
      //
      static inline bool
      digits (const string& s)
      {
        for (char c: s)
          if (!digit (c))
            return false;

        return !s.empty ();
      }

      void parser::
      parse_variable_line (token& t, type& tt)
      {
        string name (move (t.value));

        // Check if we are trying to modify any of the special aliases ($*,
        // $~, $N).
        //
        if (pre_parse_)
        {
          if (name == "*" || name == "~" || digits (name))
            fail (t) << "attempt to set '" << name << "' variable directly";
        }

        next (t, tt);
        type kind (tt); // Assignment kind.

        // We cannot reuse the value mode since it will recognize { which
        // we want to treat as a literal.
        //
        value rhs (parse_variable_value (t, tt, lexer_mode::variable_line));

        if (tt != type::newline)
          fail (t) << "unexpected " << t;

        if (!pre_parse_)
        {
          const variable& var (script_->var_pool.insert (move (name)));

          value& lhs (kind == type::assign
                      ? scope_->assign (var)
                      : scope_->append (var));

          // @@ Need to adjust to make strings the default type.
          //
          apply_value_attributes (&var, lhs, move (rhs), kind);

          // Handle the $*, $NN special aliases.
          //
          // The plan is as follows: in this function we detect modification
          // of the source variables (test*), and (re)set $* to NULL on this
          // scope (this is important to both invalidate any old values but
          // also to "stake" the lookup position). This signals to the
          // variable lookup function below that the $* and $NN values need to
          // be recalculated from their sources. Note that we don't need to
          // invalidate $NN since their lookup always checks $* first.
          //
          if (var.name == script_->test_var.name ||
              var.name == script_->opts_var.name ||
              var.name == script_->args_var.name)
          {
            scope_->assign (script_->cmd_var) = nullptr;
          }
        }
      }

      void parser::
      parse_test_line (token& t, type& tt)
      {
        test ts;

        // Pending positions where the next word should go.
        //
        enum class pending
        {
          none,
          program,
          in_string,
          in_document,
          out_string,
          out_document,
          err_string,
          err_document
        };
        pending p (pending::program);

        // Ordered sequence of here-document redirects that we can expect to
        // see after the command line.
        //
        struct here_doc
        {
          redirect* redir;
          string end;
        };
        vector<here_doc> hd;

        // Add the next word to either one of the pending positions or
        // to program arguments by default.
        //
        auto add_word = [&ts, &p, &hd, this] (string&& w, const location& l)
        {
          auto add_here_end = [&hd] (redirect& r, string&& w)
          {
            hd.push_back (here_doc {&r, move (w)});
          };

          switch (p)
          {
          case pending::none: ts.arguments.push_back (move (w)); break;
          case pending::program:
          {
            try
            {
              ts.program = path (move (w));

              if (ts.program.empty ())
                fail (l) << "empty program path";
            }
            catch (const invalid_path& e)
            {
              fail (l) << "invalid program path '" << e.path << "'";
            }
            break;
          }

          case pending::in_document:  add_here_end (ts.in,  move (w)); break;
          case pending::out_document: add_here_end (ts.out, move (w)); break;
          case pending::err_document: add_here_end (ts.err, move (w)); break;

          case pending::in_string:  ts.in.value = move (w);  break;
          case pending::out_string: ts.out.value = move (w); break;
          case pending::err_string: ts.err.value = move (w); break;
          }

          p = pending::none;
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
          case pending::out_string:   what = "stdout here-string";       break;
          case pending::out_document: what = "stdout here-document end"; break;
          case pending::err_string:   what = "stderr here-string";       break;
          case pending::err_document: what = "stderr here-document end"; break;
          }

          if (what != nullptr)
            fail (l) << "missing " << what;
        };

        // Parse the redirect operator.
        //
        auto parse_redirect =
          [&ts, &p, this] (const token& t, const location& l)
        {
          // Our semantics is the last redirect seen takes effect.
          //
          assert (p == pending::none);

          // See if we have the file descriptor.
          //
          unsigned long fd (3);
          if (!t.separated)
          {
            if (ts.arguments.empty ())
              fail (l) << "missing redirect file descriptor";

            const string& s (ts.arguments.back ());

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

            ts.arguments.pop_back ();
          }

          type tt (t.type);

          // Validate/set default file descriptor.
          //
          switch (tt)
          {
          case type::in_null:
          case type::in_string:
          case type::in_document:
            {
              if ((fd = fd == 3 ? 0 : fd) != 0)
                fail (l) << "invalid in redirect file descriptor " << fd;

              break;
            }
          case type::out_null:
          case type::out_string:
          case type::out_document:
            {
              if ((fd = fd == 3 ? 1 : fd) == 0)
                fail (l) << "invalid out redirect file descriptor " << fd;

              break;
            }
          }

          redirect_type rt;
          switch (tt)
          {
          case type::in_null:
          case type::out_null:     rt = redirect_type::null;          break;
          case type::in_string:
          case type::out_string:   rt = redirect_type::here_string;   break;
          case type::in_document:
          case type::out_document: rt = redirect_type::here_document; break;
          }

          redirect& r (fd == 0 ? ts.in : fd == 1 ? ts.out : ts.err);
          r.type = rt;

          switch (rt)
          {
          case redirect_type::none:
          case redirect_type::null:
            break;
          case redirect_type::here_string:
            switch (fd)
            {
            case 0: p = pending::in_string;  break;
            case 1: p = pending::out_string; break;
            case 2: p = pending::err_string; break;
            }
            break;
          case redirect_type::here_document:
            switch (fd)
            {
            case 0: p = pending::in_document;  break;
            case 1: p = pending::out_document; break;
            case 2: p = pending::err_document; break;
            }
            break;
          }
        };

        // Keep parsing chunks of the command line until we see the newline or
        // the exit status comparison.
        //
        location l (get_location (t));
        names ns; // Reuse to reduce allocations.

        for (bool done (false); !done; l = get_location (t))
        {
          switch (tt)
          {
          case type::equal:
          case type::not_equal:
          case type::newline:
            {
              done = true;
              break;
            }
          case type::in_null:
          case type::in_string:
          case type::in_document:
          case type::out_null:
          case type::out_string:
          case type::out_document:
            {
              if (pre_parse_)
              {
                // The only thing we need to handle here are the here-document
                // end markers since we need to know how many of the to pre-
                // parse after the command.
                //
                switch (tt)
                {
                case type::in_document:
                case type::out_document:
                  // We require the end marker to be a literal, unquoted word.
                  // In particularm, we don't allow quoted because of cases
                  // like foo"$bar" (where we will see word 'foo').
                  //
                  next (t, tt);

                  if (tt != type::word || t.quoted)
                    fail (l) << "here-document end marker expected";

                  hd.push_back (here_doc {nullptr, move (t.value)});
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
              case type::in_null:
              case type::in_string:
              case type::in_document:
              case type::out_null:
              case type::out_string:
              case type::out_document:
                parse_redirect (t, l);
                next (t, tt);
                break;
              }

              break;
            }
          default:
            {
              // Parse the next chunk as names to get variable expansion, etc.
              // Note that we do it in the chunking mode to detect whether
              // anything in each chunk is quoted.
              //
              reset_quoted (t);
              parse_names (t, tt, ns, true, "command");

              if (pre_parse_) // Nothing else to do if we are pre-parsing.
                break;

              // Process what we got. Determine whether anything inside was
              // quoted (note that the current token is "next" and is not part
              // of this).
              //
              bool q ((quoted () - (t.quoted ? 1 : 0)) != 0);

              for (name& n: ns)
              {
                string s;

                try
                {
                  s = value_traits<string>::convert (move (n), nullptr);
                }
                catch (const invalid_argument&)
                {
                  fail (l) << "invalid string value '" << n << "'";
                }

                // If it is a quoted chunk, then we add the word as is.
                // Otherwise we re-lex it. But if the word doesn't contain any
                // interesting characters (operators plus quotes/escapes),
                // then no need to re-lex.
                //
                // NOTE: updated quoting (script.cxx:to_stream_q()) if adding
                // any new characters.
                //
                if (q || s.find_first_of ("|&<>\'\"\\") == string::npos)
                  add_word (move (s), l);
                else
                {
                  // Come up with a "path" that contains both the original
                  // location as well as the expanded string. The resulting
                  // diagnostics will look like this:
                  //
                  // testscript:10:1 ('abc): unterminated single quote
                  //
                  path name;
                  {
                    string n (l.file->string ());
                    n += ':';
                    n += to_string (l.line);
                    n += ':';
                    n += to_string (l.column);
                    n += ": (";
                    n += s;
                    n += ')';
                    name = path (move (n));
                  }

                  istringstream is (s);
                  lexer lex (is, name, lexer_mode::command_line);

                  // Treat the first "sub-token" as always separated from what
                  // we saw earlier.
                  //
                  // Note that this is not "our" token so we cannot do
                  // fail(t). Rather we should do fail(l).
                  //
                  token t (lex.next ());
                  location l (build2::get_location (t, name));
                  t.separated = true;

                  string w;
                  bool f (t.type == type::eos); // If the whole thing is empty.

                  for (; t.type != type::eos; t = lex.next ())
                  {
                    type tt (t.type);
                    l = build2::get_location (t, name);

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
                    case type::in_null:
                    case type::in_string:
                    case type::out_null:
                    case type::out_string:
                      parse_redirect (t, l);
                      break;
                    case type::in_document:
                    case type::out_document:
                      fail (l) << "here-document redirect in expansion";
                      break;
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

        // Verify we don't have anything pending to be filled.
        //
        if (!pre_parse_)
          check_pending (l);

        // While we no longer need to recognize command line operators, we
        // also don't expect a valid test trailer to contain them. So we are
        // going to continue lexing in the script_line mode.
        //
        if (tt == type::equal || tt == type::not_equal)
          ts.exit = parse_command_exit (t, tt);

        if (tt != type::newline)
          fail (t) << "unexpected " << t;

        // Parse here-document fragments in the order they were mentioned on
        // the command line.
        //
        for (here_doc& h: hd)
        {
          // Switch to the here-line mode which is like double-quoted but
          // recognized the newline as a separator.
          //
          mode (lexer_mode::here_line);
          next (t, tt);

          string v (parse_here_document (t, tt, h.end));

          if (!pre_parse_)
          {
            redirect& r (*h.redir);
            r.value = move (v);
            r.here_end = move (h.end);
          }

          expire_mode ();
        }

        // Now that we have all the pieces, run the test.
        //
        if (!pre_parse_)
          runner_->run (ts);
      }

      command_exit parser::
      parse_command_exit (token& t, type& tt)
      {
        exit_comparison comp (tt == type::equal
                              ? exit_comparison::eq
                              : exit_comparison::ne);

        // The next chunk should be the exit status.
        //
        next (t, tt);
        names ns (parse_names (t, tt, true, "exit status"));
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
            fail (t) << "exit status expected instead of '" << ns << "'" <<
              info << "exit status is an unsigned integer less than 256";
        }

        return command_exit {comp, static_cast<uint8_t> (es)};
      }

      string parser::
      parse_here_document (token& t, type& tt, const string& em)
      {
        string r;

        while (tt != type::eos)
        {
          // Check if this is the end marker.
          //
          if (tt == type::word &&
              !t.quoted        &&
              t.value == em    &&
              peek () == type::newline)
          {
            next (t, tt); // Get the newline.
            break;
          }

          // Expand the line.
          //
          names ns (parse_names (t, tt, false, "here-document line"));

          if (!pre_parse_)
          {
            // What shall we do if the expansion results in multiple names?
            // For, example if the line contains just the variable expansion
            // and it is of type strings. Adding all the elements space-
            // separated seems like the natural thing to do.
            //
            for (auto b (ns.begin ()), i (b); i != ns.end (); ++i)
            {
              string s;

              try
              {
                s = value_traits<string>::convert (move (*i), nullptr);
              }
              catch (const invalid_argument&)
              {
                fail (t) << "invalid string value '" << *i << "'";
              }

              if (i != b)
                r += ' ';

              r += s;
            }

            r += '\n'; // Here-document line always includes a newline.
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

        return r;
      }

      lookup parser::
      lookup_variable (name&& qual, string&& name, const location& loc)
      {
        assert (!pre_parse_);

        if (!qual.empty ())
          fail (loc) << "qualified variable name";

        if (name != "*" && !digits (name))
          return scope_->find (script_->var_pool.insert (move (name)));

        // Handle the $*, $NN special aliases.
        //
        // See the parse_variable_line() for the overall plan.
        //

        // In both cases first thing we do is lookup $*. It should always be
        // defined since we set it on the script's root scope.
        //
        lookup l (scope_->find (script_->cmd_var));
        assert (l.defined ());

        // $* NULL value means it needs to be (re)calculated.
        //
        value& v (const_cast<value&> (*l));
        bool recalc (v.null);

        if (recalc)
        {
          strings s;

          auto append = [&s] (const strings& v)
          {
            s.insert (s.end (), v.begin (), v.end ());
          };

          if (lookup l = scope_->find (script_->test_var))
            s.push_back (cast<path> (l).string ());

          if (lookup l = scope_->find (script_->opts_var))
            append (cast<strings> (l));

          if (lookup l = scope_->find (script_->args_var))
            append (cast<strings> (l));

          v = move (s);
        }

        if (name == "*")
          return l;

        // Use the string type for the $NN variables.
        //
        const variable& var (script_->var_pool.insert<string> (move (name)));

        // We need to look for $NN in the same scope as where we found $*.
        //
        variable_map& vars (const_cast<variable_map&> (*l.vars));

        // If there is already a value and no need to recalculate it, then we
        // are done.
        //
        if (!recalc && (l = vars[var]).defined ())
          return l;

        // Convert the variable name to index we can use on $*.
        //
        unsigned long i;

        try
        {
          i = stoul (var.name);
        }
        catch (const exception&)
        {
          fail (loc) << "invalid $* index " << var.name;
        }

        const strings& s (cast<strings> (v));
        value& nv (vars.assign (var));

        if (i < s.size ())
          nv = s[i];
        else
          nv = nullptr;

        return lookup (nv, vars);
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
            if (replay_data_[i].token.quoted)
              ++r;
        }

        return r;
      }

      void parser::
      reset_quoted (token& cur)
      {
        if (replay_ != replay::play)
          lexer_->reset_quoted (cur.quoted ? 1 : 0);
        else
        {
          replay_quoted_ = replay_i_ - 1;

          // Must be the same token.
          //
          assert (replay_data_[replay_quoted_].token.quoted == cur.quoted);
        }
      }
    }
  }
}
