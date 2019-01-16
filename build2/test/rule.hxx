// file      : build2/test/rule.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TEST_RULE_HXX
#define BUILD2_TEST_RULE_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/rule.hxx>
#include <build2/action.hxx>

#include <build2/test/common.hxx>

namespace build2
{
  namespace test
  {
    class rule: public build2::rule, protected virtual common
    {
    public:
      virtual bool
      match (action, target&, const string&) const override;

      virtual recipe
      apply (action, target&) const override;

      static target_state
      perform_update (action, const target&, size_t);

      target_state
      perform_test (action, const target&, size_t) const;

      target_state
      perform_script (action, const target&, size_t) const;

      rule (common_data&& d, bool see_through_only)
          : common (move (d)), see_through (see_through_only) {}

      bool see_through;
    };

    class default_rule: public rule
    {
    public:
      explicit
      default_rule (common_data&& d)
          : common (move (d)),
            rule (move (d), true /* see_through_only */) {}
    };

    // To be used for non-see-through groups that should exhibit the see-
    // through behavior for install (see lib{} in the bin module for an
    // example).
    //
    class group_rule: public rule
    {
    public:
      explicit
      group_rule (common_data&& d)
          : common (move (d)), rule (move (d), false /* see_through_only */) {}
    };
  }
}

#endif // BUILD2_TEST_RULE_HXX
