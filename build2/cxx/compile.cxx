// file      : build2/cxx/compile.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/compile>

#include <map>
#include <limits>   // numeric_limits
#include <cstdlib>  // exit()

#include <butl/path-map>

#include <build2/depdb>
#include <build2/scope>
#include <build2/context>
#include <build2/variable>
#include <build2/algorithm>
#include <build2/diagnostics>

#include <build2/bin/target>
#include <build2/cxx/target>

#include <build2/cxx/link>
#include <build2/cxx/common>
#include <build2/cxx/utility>


using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    using namespace bin;

    match_result compile::
    match (action a, target& t, const string&) const
    {
      tracer trace ("cxx::compile::match");

      // @@ TODO:
      //
      // - check prerequisites: single source file
      // - if path already assigned, verify extension?
      //

      // See if we have a C++ source file. Iterate in reverse so that
      // a source file specified for an obj*{} member overrides the one
      // specified for the group. Also "see through" groups.
      //
      for (prerequisite_member p: reverse_group_prerequisite_members (a, t))
      {
        if (p.is_a<cxx> ())
          return p;
      }

      l4 ([&]{trace << "no c++ source file for target " << t;});
      return nullptr;
    }

    static void
    inject_prerequisites (action, target&, lorder, cxx&, scope&, depdb&);

    recipe compile::
    apply (action a, target& xt, const match_result& mr) const
    {
      tracer trace ("cxx::compile");

      file& t (static_cast<file&> (xt));

      scope& bs (t.base_scope ());
      scope& rs (*bs.root_scope ());

      const string& cid (cast<string> (rs["cxx.id"]));
      const string& tsys (cast<string> (rs["cxx.target.system"]));
      const string& tclass (cast<string> (rs["cxx.target.class"]));

      otype ct (compile_type (t));

      // Derive file name from target name.
      //
      if (t.path ().empty ())
      {
        const char* e (nullptr);

        if (tsys == "win32-msvc")
        {
          switch (ct)
          {
          case otype::e: e = "exe.obj"; break;
          case otype::a: e = "lib.obj"; break;
          case otype::s: e = "dll.obj"; break;
          }
        }
        else if (tsys == "mingw32")
        {
          switch (ct)
          {
          case otype::e: e = "exe.o"; break;
          case otype::a: e = "a.o";   break;
          case otype::s: e = "dll.o"; break;
          }
        }
        else if (tsys == "darwin")
        {
          switch (ct)
          {
          case otype::e: e = "o";       break;
          case otype::a: e = "a.o";     break;
          case otype::s: e = "dylib.o"; break;
          }
        }
        else
        {
          switch (ct)
          {
          case otype::e: e = "o"; break;
          case otype::a: e = "a.o"; break;
          case otype::s: e = "so.o"; break;
          }
        }

        t.derive_path (e);
      }

      // Inject dependency on the output directory.
      //
      fsdir* dir (inject_fsdir (a, t));

      // Search and match all the existing prerequisites. The injection
      // code (below) takes care of the ones it is adding.
      //
      // When cleaning, ignore prerequisites that are not in the same
      // or a subdirectory of our project root.
      //
      optional<dir_paths> lib_paths; // Extract lazily.

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // A dependency on a library is there so that we can get its
        // cxx.export.poptions. In particular, making sure it is
        // executed before us will only restrict parallelism. But we
        // do need to pre-match it in order to get its
        // prerequisite_targets populated. This is the "library
        // meta-information protocol". See also append_lib_options()
        // above.
        //
        if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libs> ())
        {
          if (a.operation () == update_id)
          {
            // Handle imported libraries. We know that for such libraries
            // we don't need to do match() in order to get options (if
            // any, they would be set by search_library()).
            //
            if (p.proj () == nullptr ||
                link::search_library (lib_paths, p.prerequisite) == nullptr)
            {
              match_only (a, p.search ());
            }
          }

          continue;
        }

        target& pt (p.search ());

        if (a.operation () == clean_id && !pt.dir.sub (rs.out_path ()))
          continue;

        build2::match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }

      // Inject additional prerequisites. We only do it when performing update
      // since chances are we will have to update some of our prerequisites in
      // the process (auto-generated source code).
      //
      if (a == perform_update_id)
      {
        // The cached prerequisite target should be the same as what is in
        // t.prerequisite_targets since we used standard search() and match()
        // above.
        //
        // @@ Ugly.
        //
        cxx& st (
          dynamic_cast<cxx&> (
            mr.target != nullptr ? *mr.target : *mr.prerequisite->target));

        // Make sure the output directory exists.
        //
        // Is this the right thing to do? It does smell a bit, but then we do
        // worse things in inject_prerequisites() below. There is also no way
        // to postpone this until update since we need to extract and inject
        // header dependencies now (we don't want to be calling search() and
        // match() in update), which means we need to cache them now as well.
        // So the only alternative, it seems, is to cache the updates to the
        // database until later which will sure complicate (and slow down)
        // things.
        //
        if (dir != nullptr)
          execute_direct (a, *dir);

        depdb dd (t.path () + ".d");

        // First should come the rule name/version.
        //
        if (dd.expect ("cxx.compile 1") != nullptr)
          l4 ([&]{trace << "rule mismatch forcing update of " << t;});

        // Then the compiler checksum.
        //
        if (dd.expect (cast<string> (rs["cxx.checksum"])) != nullptr)
          l4 ([&]{trace << "compiler mismatch forcing update of " << t;});

        // Then the options checksum.
        //
        // The idea is to keep them exactly as they are passed to the compiler
        // since the order may be significant.
        //
        sha256 cs;

        // Hash cxx.export.poptions from prerequisite libraries.
        //
        lorder lo (link_order (bs, ct));
        for (prerequisite& p: group_prerequisites (t))
        {
          target* pt (p.target); // Already searched and matched.

          if (lib* l = pt->is_a<lib> ())
            pt = &link_member (*l, lo);

          if (pt->is_a<liba> () || pt->is_a<libs> ())
            hash_lib_options (cs, *pt, "cxx.export.poptions", lo);
        }

        hash_options (cs, t, "cxx.poptions");
        hash_options (cs, t, "cxx.coptions");
        hash_std (cs, rs, cid, t);

        if (ct == otype::s)
        {
          // On Darwin, Win32 -fPIC is the default.
          //
          if (tclass == "linux" || tclass == "freebsd")
            cs.append ("-fPIC");
        }

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "options mismatch forcing update of " << t;});

        // Finally the source file.
        //
        if (dd.expect (st.path ()) != nullptr)
          l4 ([&]{trace << "source file mismatch forcing update of " << t;});

        // If any of the above checks resulted in a mismatch (different
        // compiler, options, or source file), or if the database is newer
        // than the target (interrupted update) then force the target update.
        //
        if (dd.writing () || dd.mtime () > t.mtime ())
          t.mtime (timestamp_nonexistent);

        inject_prerequisites (a, t, lo, st, mr.prerequisite->scope, dd);

        dd.close ();
      }

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id: return &perform_clean;
      default: return noop_recipe; // Configure update.
      }
    }

    // Reverse-lookup target type from extension.
    //
    static const target_type*
    map_extension (scope& s, const string& n, const string& e)
    {
      // We will just have to try all of the possible ones, in the
      // "most likely to match" order.
      //
      const variable& var (var_pool.find ("extension"));

      auto test = [&s, &n, &e, &var] (const target_type& tt)
        -> const target_type*
      {
        if (auto l = s.find (var, tt, n))
          if (cast<string> (l) == e)
            return &tt;

        return nullptr;
      };

      if (auto r = test (hxx::static_type)) return r;
      if (auto r = test (h::static_type))   return r;
      if (auto r = test (ixx::static_type)) return r;
      if (auto r = test (txx::static_type)) return r;
      if (auto r = test (cxx::static_type)) return r;
      if (auto r = test (c::static_type))   return r;

      return nullptr;
    }

    // Mapping of include prefixes (e.g., foo in <foo/bar>) for auto-
    // generated headers to directories where they will be generated.
    //
    // We are using a prefix map of directories (dir_path_map) instead
    // of just a map in order also cover sub-paths (e.g., <foo/more/bar>
    // if we continue with the example). Specifically, we need to make
    // sure we don't treat foobar as a sub-directory of foo.
    //
    // @@ The keys should be canonicalized.
    //
    using prefix_map = dir_path_map<dir_path>;

    static void
    append_prefixes (prefix_map& m, target& t, const char* var)
    {
      tracer trace ("cxx::append_prefixes");

      // If this target does not belong to any project (e.g, an
      // "imported as installed" library), then it can't possibly
      // generate any headers for us.
      //
      scope* rs (t.base_scope ().root_scope ());
      if (rs == nullptr)
        return;

      const dir_path& out_base (t.dir);
      const dir_path& out_root (rs->out_path ());

      if (auto l = t[var])
      {
        const auto& v (cast<strings> (l));

        for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
        {
          // -I can either be in the "-Ifoo" or "-I foo" form. For VC it can
          // also be /I.
          //
          const string& o (*i);

          if (o.size () < 2 || (o[0] != '-' && o[0] != '/') || o[1] != 'I')
            continue;

          dir_path d;
          if (o.size () == 2)
          {
            if (++i == e)
              break; // Let the compiler complain.

            d = dir_path (*i);
          }
          else
            d = dir_path (*i, 2, string::npos);

          l6 ([&]{trace << "-I '" << d << "'";});

          // If we are relative or not inside our project root, then
          // ignore.
          //
          if (d.relative () || !d.sub (out_root))
            continue;

          // If the target directory is a sub-directory of the include
          // directory, then the prefix is the difference between the
          // two. Otherwise, leave it empty.
          //
          // The idea here is to make this "canonical" setup work auto-
          // magically:
          //
          // 1. We include all files with a prefix, e.g., <foo/bar>.
          // 2. The library target is in the foo/ sub-directory, e.g.,
          //    /tmp/foo/.
          // 3. The poptions variable contains -I/tmp.
          //
          dir_path p (out_base.sub (d) ? out_base.leaf (d) : dir_path ());

          auto j (m.find (p));

          if (j != m.end ())
          {
            if (j->second != d)
            {
              // We used to reject duplicates but it seems this can
              // be reasonably expected to work according to the order
              // of the -I options.
              //
              if (verb >= 4)
                trace << "overriding dependency prefix '" << p << "'\n"
                      << "  old mapping to " << j->second << "\n"
                      << "  new mapping to " << d;

              j->second = d;
            }
          }
          else
          {
            l6 ([&]{trace << "'" << p << "' = '" << d << "'";});
            m.emplace (move (p), move (d));
          }
        }
      }
    }

    // Append library prefixes based on the cxx.export.poptions variables
    // recursively, prerequisite libraries first.
    //
    static void
    append_lib_prefixes (prefix_map& m, target& l, lorder lo)
    {
      for (target* t: l.prerequisite_targets)
      {
        if (t == nullptr)
          continue;

        if (lib* l = t->is_a<lib> ())
          t = &link_member (*l, lo); // Pick one of the members.

        if (t->is_a<liba> () || t->is_a<libs> ())
          append_lib_prefixes (m, *t, lo);
      }

      append_prefixes (m, l, "cxx.export.poptions");
    }

    static prefix_map
    build_prefix_map (target& t, lorder lo)
    {
      prefix_map m;

      // First process the include directories from prerequisite
      // libraries. Note that here we don't need to see group
      // members (see apply()).
      //
      for (prerequisite& p: group_prerequisites (t))
      {
        target* pt (p.target); // Already searched and matched.

        if (lib* l = pt->is_a<lib> ())
          pt = &link_member (*l, lo); // Pick one of the members.

        if (pt->is_a<liba> () || pt->is_a<libs> ())
          append_lib_prefixes (m, *pt, lo);
      }

      // Then process our own.
      //
      append_prefixes (m, t, "cxx.poptions");

      return m;
    }

    // Return the next make prerequisite starting from the specified
    // position and update position to point to the start of the
    // following prerequisite or l.size() if there are none left.
    //
    static string
    next_make (const string& l, size_t& p)
    {
      size_t n (l.size ());

      // Skip leading spaces.
      //
      for (; p != n && l[p] == ' '; p++) ;

      // Lines containing multiple prerequisites are 80 characters max.
      //
      string r;
      r.reserve (n);

      // Scan the next prerequisite while watching out for escape sequences.
      //
      for (; p != n && l[p] != ' '; p++)
      {
        char c (l[p]);

        if (p + 1 != n)
        {
          if (c == '$')
          {
            // Got to be another (escaped) '$'.
            //
            if (l[p + 1] == '$')
              ++p;
          }
          else if (c == '\\')
          {
            // This may or may not be an escape sequence depending on whether
            // what follows is "escapable".
            //
            switch (c = l[++p])
            {
            case '\\': break;
            case ' ': break;
            default: c = '\\'; --p; // Restore.
            }
          }
        }

        r += c;
      }

      // Skip trailing spaces.
      //
      for (; p != n && l[p] == ' '; p++) ;

      // Skip final '\'.
      //
      if (p == n - 1 && l[p] == '\\')
        p++;

      return r;
    }

    // Extract the include path from the VC++ /showIncludes output line.
    // Return empty string if the line is not an include note or include
    // error. Set the good_error flag if it is an include error (which means
    // the process will terminate with the error status that needs to be
    // ignored).
    //
    static string
    next_show (const string& l, bool& good_error)
    {
      // The include error should be the last line that we handle.
      //
      assert (!good_error);

      // VC++ /showIncludes output. The first line is the file being
      // compiled. Then we have the list of headers, one per line, in this
      // form (text can presumably be translated):
      //
      // Note: including file: C:\Program Files (x86)\[...]\iostream
      //
      // Finally, if we hit a non-existent header, then we end with an error
      // line in this form:
      //
      // x.cpp(3): fatal error C1083: Cannot open include file: 'd/h.hpp':
      // No such file or directory
      //

      // Distinguishing between the include note and the include error is
      // easy: we can just check for C1083. Distinguising between the note and
      // other errors/warnings is harder: an error could very well end with
      // what looks like a path so we cannot look for the note but rather have
      // to look for an error. Here we assume that a line containing ' CNNNN:'
      // is an error. Should be robust enough in the face of language
      // translation, etc.
      //
      size_t p (l.find (':'));
      size_t n (l.size ());

      for (; p != string::npos; p = ++p != n ? l.find (':', p) : string::npos)
      {
        auto isnum = [](char c) {return c >= '0' && c <= '9';};

        if (p > 5 &&
            l[p - 6] == ' '  &&
            l[p - 5] == 'C'  &&
            isnum (l[p - 4]) &&
            isnum (l[p - 3]) &&
            isnum (l[p - 2]) &&
            isnum (l[p - 1]))
        {
          p -= 4; // Start of the error code.
          break;
        }
      }

      if (p == string::npos)
      {
        // Include note. We assume the path is always at the end but
        // need to handle both absolute Windows and POSIX ones.
        //
        size_t p (l.rfind (':'));

        if (p != string::npos)
        {
          // See if this one is part of the Windows drive letter.
          //
          auto isalpha = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');};

          if (p > 1 && p + 1 < n && // 2 chars before, 1 after.
              l[p - 2] == ' ' &&
              isalpha (l[p - 1]) &&
              path::traits::is_separator (l[p + 1]))
            p = l.rfind (':', p - 2);
        }

        if (p != string::npos)
        {
          // VC uses indentation to indicate the include nesting so there
          // could be any number of spaces after ':'. Skip them.
          //
          p = l.find_first_not_of (' ', p + 1);
        }

        if (p == string::npos)
          fail << "unable to parse /showIncludes include note line";

        return string (l, p);
      }
      else if (l.compare (p, 4, "1083") == 0)
      {
        // Include error. The path is conveniently quoted with ''.
        //
        size_t p2 (l.rfind ('\''));

        if (p2 != string::npos && p2 != 0)
        {
          size_t p1 (l.rfind ('\'', p2 - 1));

          if (p1 != string::npos)
          {
            good_error = true;
            return string (l, p1 + 1 , p2 - p1 - 1);
          }
        }

        error << "unable to parse /showIncludes include error line";
        throw failed ();
      }
      else
      {
        // Some other error.
        //
        return string ();
      }
    }

    static void
    inject_prerequisites (action a, target& t, lorder lo,
                          cxx& s, scope& ds, depdb& dd)
    {
      tracer trace ("cxx::compile::inject_prerequisites");

      l6 ([&]{trace << "target: " << t;});

      // If things go wrong (and they often do in this area), give the user a
      // bit extra context.
      //
      auto g (
        make_exception_guard (
          [&s]()
          {
            info << "while extracting header dependencies from " << s;
          }));

      scope& rs (t.root_scope ());
      const string& cid (cast<string> (rs["cxx.id"]));

      // Initialize lazily, only if required.
      //
      cstrings args;
      string cxx_std; // Storage.

      auto init_args = [&t, lo, &s, &rs, &cid, &args, &cxx_std] ()
      {
        const path& cxx (cast<path> (rs["config.cxx"]));
        const string& tclass (cast<string> (rs["cxx.target.class"]));

        args.push_back (cxx.string ().c_str ());

        // Add cxx.export.poptions from prerequisite libraries. Note
        // that here we don't need to see group members (see apply()).
        //
        for (prerequisite& p: group_prerequisites (t))
        {
          target* pt (p.target); // Already searched and matched.

          if (lib* l = pt->is_a<lib> ())
            pt = &link_member (*l, lo);

          if (pt->is_a<liba> () || pt->is_a<libs> ())
            append_lib_options (args, *pt, "cxx.export.poptions", lo);
        }

        append_options (args, t, "cxx.poptions");

        // Some C++ options (e.g., -std, -m) affect the preprocessor.
        //
        append_options (args, t, "cxx.coptions");
        append_std (args, rs, cid, t, cxx_std);

        if (t.is_a<objs> ())
        {
          // On Darwin, Win32 -fPIC is the default.
          //
          if (tclass == "linux" || tclass == "freebsd")
            args.push_back ("-fPIC");
        }

        if (cid == "msvc")
        {
          args.push_back ("/nologo");
          args.push_back ("/EP");           // Preprocess to stdout.
          args.push_back ("/TP");           // Preprocess as C++.
          args.push_back ("/showIncludes"); // Goes to sterr becasue of /EP.
        }
        else
        {
          args.push_back ("-M");  // Note: -MM -MG skips missing <>-included.
          args.push_back ("-MG"); // Treat missing headers as generated.

          // Previously we used '*' as a target name but it gets expanded to
          // the current directory file names by GCC (4.9) that comes with
          // MSYS2 (2.4). Yes, this is the (bizarre) behavior of GCC being
          // executed in the shell with -MQ '*' option and not just -MQ *.
          //
          args.push_back ("-MQ"); // Quoted target name.
          args.push_back ("^");   // Old versions can't do empty target name.
        }

        // We are using absolute source file path in order to get absolute
        // paths in the result. Any relative paths in the result are non-
        // existent, potentially auto-generated headers.
        //
        // @@ We will also have to use absolute -I paths to guarantee
        // that. Or just detect relative paths and error out?
        //
        args.push_back (s.path ().string ().c_str ());
        args.push_back (nullptr);
      };

      // Build the prefix map lazily only if we have non-existent files.
      // Also reuse it over restarts since it doesn't change.
      //
      prefix_map pm;

      // If any prerequisites that we have extracted changed, then we have to
      // redo the whole thing. The reason for this is auto-generated headers:
      // the updated header may now include a yet-non-existent header. Unless
      // we discover this and generate it (which, BTW, will trigger another
      // restart since that header, in turn, can also include auto-generated
      // headers), we will end up with an error during compilation proper.
      //
      // One complication with this restart logic is that we will see a
      // "prefix" of prerequisites that we have already processed (i.e., they
      // are already in our prerequisite_targets list) and we don't want to
      // keep redoing this over and over again. One thing to note, however, is
      // that the prefix that we have seen on the previous run must appear
      // exactly the same in the subsequent run. The reason for this is that
      // none of the files that it can possibly be based on have changed and
      // thus it should be exactly the same. To put it another way, the
      // presence or absence of a file in the dependency output can only
      // depend on the previous files (assuming the compiler outputs them as
      // it encounters them and it is hard to think of a reason why would
      // someone do otherwise). And we have already made sure that all those
      // files are up to date. And here is the way we are going to exploit
      // this: we are going to keep track of how many prerequisites we have
      // processed so far and on restart skip right to the next one.
      //
      // And one more thing: most of the time this list of headers would stay
      // unchanged and extracting them by running the compiler every time is a
      // bit wasteful. So we are going to cache them in the depdb. If the db
      // hasn't been invalidated yet (e.g., because the compiler options have
      // changed), then we start by reading from it. If anything is out of
      // date then we use the same restart and skip logic to switch to the
      // compiler run.
      //

      // Update the target "smartly". Return true if it has changed or if the
      // passed timestamp is not timestamp_unknown and is older than the
      // target.
      //
      // There would normally be a lot of headers for every source file (think
      // all the system headers) and just calling execute_direct() on all of
      // them can get expensive. At the same time, most of these headers are
      // existing files that we will never be updating (again, system headers,
      // for example) and the rule that will match them is the fallback
      // file_rule. That rule has an optimization: it returns noop_recipe
      // (which causes the target state to be automatically set to unchanged)
      // if the file is known to be up to date.
      //
      auto update = [&trace, a] (path_target& pt, timestamp ts) -> bool
      {
        if (pt.state () != target_state::unchanged)
        {
          // We only want to restart if our call to execute() actually
          // caused an update. In particular, the target could already
          // have been in target_state::changed because of a dependency
          // extraction run for some other source file.
          //
          target_state os (pt.state ());
          target_state ns (execute_direct (a, pt));

          if (ns != os && ns != target_state::unchanged)
          {
            l6 ([&]{trace << "updated " << pt
                          << "; old state " << os
                          << "; new state " << ns;});
            return true;
          }
        }

        if (ts != timestamp_unknown)
        {
          timestamp mt (pt.mtime ());

          // See execute_prerequisites() for rationale behind the equal part.
          //
          return ts < mt || (ts == mt && pt.state () != target_state::changed);
        }

        return false;
      };

      // Update and add a header file to the list of prerequisite targets.
      // Depending on the cache flag, the file is assumed to either have come
      // from the depdb cache or from the compiler run. Return whether the
      // extraction process should be restarted.
      //
      auto add = [&trace, &update, &pm, a, &t, lo, &ds, &dd]
        (path f, bool cache) -> bool
      {
        if (!f.absolute ())
        {
          f.normalize ();

          // This is probably as often an error as an auto-generated file, so
          // trace at level 4.
          //
          l4 ([&]{trace << "non-existent header '" << f << "'";});

          // If we already did this and build_prefix_map() returned empty,
          // then we would have failed below.
          //
          if (pm.empty ())
            pm = build_prefix_map (t, lo);

          // First try the whole file. Then just the directory.
          //
          // @@ Has to be a separate map since the prefix can be
          //    the same as the file name.
          //
          // auto i (pm.find (f));

          // Find the most qualified prefix of which we are a sub-path.
          //
          auto i (pm.end ());

          if (!pm.empty ())
          {
            const dir_path& d (f.directory ());
            i = pm.upper_bound (d);

            // Get the greatest less than, if any. We might still not be a
            // sub. Note also that we still have to check the last element if
            // upper_bound() returned end().
            //
            if (i == pm.begin () || !d.sub ((--i)->first))
              i = pm.end ();
          }

          if (i == pm.end ())
            fail << "unable to map presumably auto-generated header '"
                 << f << "' to a project";

          f = i->second / f;
        }
        else
        {
          // We used to just normalize the path but that could result in an
          // invalid path (e.g., on CentOS 7 with Clang 3.4) because of the
          // symlinks. So now we realize (i.e., realpath(3)) it instead. If
          // it comes from the depdb, in which case we've already done that.
          //
          if (!cache)
            f.realize ();
        }

        l6 ([&]{trace << "injecting " << f;});

        // Split the name into its directory part, the name part, and
        // extension. Here we can assume the name part is a valid filesystem
        // name.
        //
        // Note that if the file has no extension, we record an empty
        // extension rather than NULL (which would signify that the default
        // extension should be added).
        //
        dir_path d (f.directory ());
        string n (f.leaf ().base ().string ());
        const char* es (f.extension ());
        const string* e (&extension_pool.find (es != nullptr ? es : ""));

        // Determine the target type.
        //
        const target_type* tt (nullptr);

        // See if this directory is part of any project out_root hierarchy.
        // Note that this will miss all the headers that come from src_root
        // (so they will be treated as generic C headers below). Generally,
        // we don't have the ability to determine that some file belongs to
        // src_root of some project. But that's not a problem for our
        // purposes: it is only important for us to accurately determine
        // target types for headers that could be auto-generated.
        //
        // While at it also try to determine if this target is from the src
        // or out tree of said project.
        //
        dir_path out;

        scope& bs (scopes.find (d));
        if (scope* rs = bs.root_scope ())
        {
          tt = map_extension (bs, n, *e);

          if (bs.out_path () != bs.src_path () && d.sub (bs.src_path ()))
            out = out_src (d, *rs);
        }

        // If it is outside any project, or the project doesn't have
        // such an extension, assume it is a plain old C header.
        //
        if (tt == nullptr)
          tt = &h::static_type;

        // Find or insert target.
        //
        // @@ OPT: move d, out, n
        //
        path_target& pt (
          static_cast<path_target&> (search (*tt, d, out, n, e, &ds)));

        // Assign path.
        //
        if (pt.path ().empty ())
          pt.path (move (f));
        else
          assert (pt.path () == f);

        // Match to a rule.
        //
        build2::match (a, pt);

        // Update.
        //
        // If this header came from the depdb, make sure it is no older than
        // the db itself (if it has changed since the db was written, then
        // chances are the cached data is stale).
        //
        bool restart (update (pt, cache ? dd.mtime () : timestamp_unknown));

        // Verify/add it to the dependency database. We do it after update in
        // order not to add bogus files (non-existent and without a way to
        // update).
        //
        if (!cache)
          dd.expect (pt.path ());

        // Add to our prerequisite target list.
        //
        t.prerequisite_targets.push_back (&pt);

        return restart;
      };

      // If nothing so far has invalidated the dependency database, then
      // try the cached data before running the compiler.
      //
      bool cache (dd.reading ());

      // But, before we do all that, make sure the source file itself if up to
      // date.
      //
      if (update (s, dd.mtime ()))
      {
        // If the file got updated or is newer than the database, then we
        // cannot rely on the cache any further. However, the cached data
        // could actually still be valid so the compiler run will validate it.
        //
        // We do need to update the database timestamp, however. Failed that,
        // we will keep re-validating the cached data over and over again.
        //
        if (cache)
        {
          cache = false;
          dd.touch ();
        }
      }

      size_t skip_count (0);
      for (bool restart (true); restart; cache = false)
      {
        restart = false;

        if (cache)
        {
          // If any, this is always the first run.
          //
          assert (skip_count == 0);

          while (dd.more ())
          {
            string* l (dd.read ());

            // If the line is invalid, run the compiler.
            //
            if (l == nullptr)
            {
              restart = true;
              break;
            }

            restart = add (path (move (*l)), true);
            skip_count++;

            // The same idea as in the source file update above.
            //
            if (restart)
            {
              l6 ([&]{trace << "restarting";});
              dd.touch ();
              break;
            }
          }
        }
        else
        {
          try
          {
            if (args.empty ())
              init_args ();

            if (verb >= 3)
              print_process (args);

            // For VC with /EP we need a pipe to stderr and stdout should go
            // to /dev/null.
            //
            process pr (args.data (),
                        0,
                        cid == "msvc" ? -2 : -1,
                        cid == "msvc" ? -1 : 2);

            ifdstream is (cid == "msvc" ? pr.in_efd : pr.in_ofd,
                          fdtranslate::text);

            // In some cases we may need to ignore the error return
            // status. The good_error flag keeps track of that. Similarly
            // we sometimes expect the error return status based on the
            // output we see. The bad_error flag is for that.
            //
            bool good_error (false), bad_error (false);

            size_t skip (skip_count);
            for (bool first (true), second (false); !(restart || is.eof ()); )
            {
              string l;
              getline (is, l);

              if (is.fail ())
              {
                if (is.eof ()) // Trailing newline.
                  break;

                fail << "unable to read C++ compiler header dependency output";
              }

              l6 ([&]{trace << "header dependency line '" << l << "'";});

              // Parse different dependency output formats.
              //
              if (cid == "msvc")
              {
                if (first)
                {
                  // The first line should be the file we are compiling. If it
                  // is not, then something went wrong even before we could
                  // compile anything (e.g., file does not exist). In this
                  // case the first line (and everything after it) is
                  // presumably diagnostics.
                  //
                  if (l != s.path ().leaf ().string ())
                  {
                    text << l;
                    bad_error = true;
                    break;
                  }

                  first = false;
                  continue;
                }

                string f (next_show (l, good_error));

                if (f.empty ()) // Some other diagnostics.
                {
                  text << l;
                  bad_error = true;
                  break;
                }

                // Skip until where we left off.
                //
                if (skip != 0)
                {
                  // We can't be skipping over a non-existent header.
                  //
                  assert (!good_error);
                  skip--;
                }
                else
                {
                  restart = add (path (move (f)), false);
                  skip_count++;

                  // If the header does not exist, we better restart.
                  //
                  assert (!good_error || restart);

                  if (restart)
                    l6 ([&]{trace << "restarting";});
                }
              }
              else
              {
                // Make dependency declaration.
                //
                size_t pos (0);

                if (first)
                {
                  // Empty output should mean the wait() call below will
                  // return false.
                  //
                  if (l.empty ())
                  {
                    bad_error = true;
                    break;
                  }

                  assert (l[0] == '^' && l[1] == ':' && l[2] == ' ');

                  first = false;
                  second = true;

                  // While normally we would have the source file on the first
                  // line, if too long, it will be moved to the next line and
                  // all we will have on this line is "^: \".
                  //
                  if (l.size () == 4 && l[3] == '\\')
                    continue;
                  else
                    pos = 3; // Skip "^: ".

                  // Fall through to the 'second' block.
                }

                if (second)
                {
                  second = false;
                  next_make (l, pos); // Skip the source file.
                }

                while (pos != l.size ())
                {
                  string f (next_make (l, pos));

                  // Skip until where we left off.
                  //
                  if (skip != 0)
                  {
                    skip--;
                    continue;
                  }

                  restart = add (path (move (f)), false);
                  skip_count++;

                  if (restart)
                  {
                    l6 ([&]{trace << "restarting";});
                    break;
                  }
                }
              }
            }

            // We may not have read all the output (e.g., due to a restart).
            // Before we used to just close the file descriptor to signal to
            // the other end that we are not interested in the rest. This
            // works fine with GCC but Clang (3.7.0) finds this impolite and
            // complains, loudly (broken pipe). So now we are going to skip
            // until the end.
            //
            // Also, in case of VC++, we are parsing stderr and if things go
            // south, we need to copy the diagnostics for the user to see.
            //
            if (!is.eof ())
            {
              if (cid == "msvc" && bad_error)
                *diag_stream << is.rdbuf ();
              else
                is.ignore (numeric_limits<streamsize>::max ());
            }

            is.close ();

            // We assume the child process issued some diagnostics.
            //
            if (!pr.wait ())
            {
              if (!good_error) // Ignore expected errors (restart).
                throw failed ();
            }
            else if (bad_error)
              fail << "expected error exist status from C++ compiler";

          }
          catch (const process_error& e)
          {
            error << "unable to execute " << args[0] << ": " << e.what ();

            // In a multi-threaded program that fork()'ed but did not exec(),
            // it is unwise to try to do any kind of cleanup (like unwinding
            // the stack and running destructors).
            //
            if (e.child ())
              exit (1);

            throw failed ();
          }
        }
      }
    }

    target_state compile::
    perform_update (action a, target& xt)
    {
      file& t (static_cast<file&> (xt));
      cxx* s (execute_prerequisites<cxx> (a, t, t.mtime ()));

      if (s == nullptr)
        return target_state::unchanged;

      scope& bs (t.base_scope ());
      scope& rs (*bs.root_scope ());

      const path& cxx (cast<path> (rs["config.cxx"]));
      const string& cid (cast<string> (rs["cxx.id"]));
      const string& tclass (cast<string> (rs["cxx.target.class"]));

      otype ct (compile_type (t));

      cstrings args {cxx.string ().c_str ()};

      // Translate paths to relative (to working directory) ones. This
      // results in easier to read diagnostics.
      //
      path relo (relative (t.path ()));
      path rels (relative (s->path ()));

      // Add cxx.export.poptions from prerequisite libraries. Note that
      // here we don't need to see group members (see apply()).
      //
      lorder lo (link_order (bs, ct));
      for (prerequisite& p: group_prerequisites (t))
      {
        target* pt (p.target); // Already searched and matched.

        if (lib* l = pt->is_a<lib> ())
          pt = &link_member (*l, lo);

        if (pt->is_a<liba> () || pt->is_a<libs> ())
          append_lib_options (args, *pt, "cxx.export.poptions", lo);
      }

      append_options (args, t, "cxx.poptions");
      append_options (args, t, "cxx.coptions");

      string std, out, out1; // Storage.

      append_std (args, rs, cid, t, std);

      if (cid == "msvc")
      {
        uint64_t cver (cast<uint64_t> (rs["cxx.version.major"]));

        if (verb < 3)
          args.push_back ("/nologo");

        // The /F*: option variants with separate names only became available
        // in VS2013/12.0. Why do we bother? Because the command line suddenly
        // becomes readable.

        // The presence of /Zi or /ZI causes the compiler to write debug info
        // to the .pdb file. By default it is a shared file called vcNN.pdb
        // (where NN is the VC version) created (wait for it) in the current
        // working directory (and not the directory of the .obj file). Also,
        // because it is shared, there is a special Windows service that
        // serializes access. We, of course, want none of that so we will
        // create a .pdb per object file.
        //
        // Note that this also changes the name of the .idb file (used for
        // minimal rebuild and incremental compilation): cl.exe take the /Fd
        // value and replaces the .pdb extension with .idb.
        //
        // Note also that what we are doing here appears to be incompatible
        // with PCH (/Y* options) and /Gm (minimal rebuild).
        //
        if (find_option ("/Zi", args) || find_option ("/ZI", args))
        {
          if (cver >= 18)
            args.push_back ("/Fd:");
          else
            out1 = "/Fd";

          out1 += relo.string ();
          out1 += ".pdb";

          args.push_back (out1.c_str ());
        }

        if (cver >= 18)
        {
          args.push_back ("/Fo:");
          args.push_back (relo.string ().c_str ());
        }
        else
        {
          out = "/Fo" + relo.string ();
          args.push_back (out.c_str ());
        }

        args.push_back ("/c");  // Compile only.
        args.push_back ("/TP"); // Compile as C++.
        args.push_back (rels.string ().c_str ());
      }
      else
      {
        if (ct == otype::s)
        {
          // On Darwin, Win32 -fPIC is the default.
          //
          if (tclass == "linux" || tclass == "freebsd")
            args.push_back ("-fPIC");
        }

        args.push_back ("-o");
        args.push_back (relo.string ().c_str ());

        args.push_back ("-c");
        args.push_back (rels.string ().c_str ());
      }

      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "c++ " << *s;

      try
      {
        // @@ VC prints file name being compiled to stdout as the first
        //    line, would be good to weed it out (but check if it is
        //    always printed, for example if the file does not exist).
        //    Seems always. The same story with link.exe when creating
        //    the DLL.
        //

        // VC++ cl.exe sends diagnostics to stdout. To fix this (and any other
        // insane compilers that may want to do something like this) we are
        // going to always redirect stdout to stderr. For sane compilers this
        // should be harmless.
        //
        process pr (args.data (), 0, 2);

        if (!pr.wait ())
          throw failed ();

        // Should we go to the filesystem and get the new mtime? We
        // know the file has been modified, so instead just use the
        // current clock time. It has the advantage of having the
        // subseconds precision.
        //
        t.mtime (system_clock::now ());
        return target_state::changed;
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    target_state compile::
    perform_clean (action a, target& xt)
    {
      file& t (static_cast<file&> (xt));

      scope& rs (t.root_scope ());
      const string& cid (cast<string> (rs["cxx.id"]));

      initializer_list<const char*> e;

      if (cid == "msvc")
        e = {".d", ".idb", ".pdb"};
      else
        e = {".d"};

      return clean_extra (a, t, e);
    }

    compile compile::instance;
  }
}
