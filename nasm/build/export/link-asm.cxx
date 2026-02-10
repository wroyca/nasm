// c++ 1 //---

#include <libbuild2/bin/target.hxx>

using namespace build2::bin;

//---

// namespace build2
// {
//   class rule: public simple_rule
//   {

bool
match (action a, target& t) const
{
  tracer trace ("nasm.link::match");

  const scope& rs (*t.base_scope ().root_scope ());
  const target_type* at (rs.find_target_type ("asm"));

  if (at == nullptr)
    return false;

  // If this is an executable, we don't want to take over the linking process
  // completely. That is, the standard cc link rule is perfectly capable of
  // handling this if we feed it object files.
  //
  // So instead of matching, we synthesize obje{} targets from our asm{}
  // sources, replace the latter with the former in the executable's
  // prerequisite list, and return false. The cc link rule will match (later)
  // and see the obje{} as standard object dependencies.
  //
  // Note that the target is locked during match so messing with prerequisites
  // is safe.
  //
  if (t.is_a<exe> ())
  {
    context& ctx (t.ctx);
    const target_type* et (rs.find_target_type ("obje"));

    if (et == nullptr)
      return false;

    // Scan for asm{} prerequisites.
    //
    prerequisites ops;

    for (prerequisite_member p: group_prerequisite_members (a, t))
    {
      if (include (a, t, p) != include_type::normal)
        continue;

      if (!p.is_a (*at))
        continue;

      const target& as (p.search (t));

      // Synthesize the object name. Tack on ".asm" to the stem to prevent
      // collisions with C/C++ objects (e.g., foo.cxx and foo.asm).
      //
      string n (as.name + ".asm");

      pair<target&, ulock> l (
        search_new_locked (ctx, *et, t.dir, dir_path (), move (n)));

      // If we created a new target, set its prerequisite to the asm source.
      //
      if (l.second.owns_lock ())
      {
        l.first.prerequisites (prerequisites {prerequisite (as, true)});
        l.second.unlock ();
      }

      ops.push_back (prerequisite (l.first, true));
    }

    // If we found any asm sources, perform the switcheroo. We filter out the
    // asm{} prerequisites and append the synthesized obje{} ones.
    //
    if (!ops.empty ())
    {
      auto& ps (const_cast<target::prerequisites_type&> (t.prerequisites ()));
      prerequisites nps;

      for (auto& p: ps)
      {
        if (!p.is_a (*at))
          nps.push_back (move (p));
      }

      for (auto& p: ops)
        nps.push_back (move (p));

      ps = move (nps);
    }

    return false;
  }

  // For libraries, the situation is different. A lib{} is a group target that
  // usually delegates to liba{} (static) and libs{} (shared) members. The cc
  // link rule for lib{} delegates to these members.
  //
  // We can't easily "modify and retreat" here because we need to inject
  // objects into the *members*, which might not exist yet or haven't been
  // discovered. So we claim the target and handle the distribution in apply().
  //
  for (prerequisite_member p: group_prerequisite_members (a, t))
  {
    if (include (a, t, p) != include_type::normal)
      continue;

    if (p.is_a (*at))
      return true;
  }

  l4 ([&]{trace << "no asm{} prerequisite for target " << t;});
  return false;
}

virtual recipe
apply (action a, target& xt, match_extra&) const override
{
  tracer trace ("nasm.link::apply");

  context& ctx (xt.ctx);
  const scope& rs (*xt.base_scope ().root_scope ());
  const target_type& at (*rs.find_target_type ("asm"));

  lib& t (xt.as<lib> ());

  // Figure out which members we are building. This logic mirrors the standard
  // bin.lib behavior. Note that for distribution (install/dist) we generally
  // want to build everything.
  //
  bool ma, ms;
  {
    const string& s (cast<string> (rs["bin.lib"]));

    ma = (s == "static" || s == "both");
    ms = (s == "shared" || s == "both");

    if (a.meta_operation () == dist_id)
      ma = ms = true;
  }

  // We need the member types to find the specific library members (liba/libs)
  // and the object types (obja/objs) to synthesize the corresponding object
  // targets.
  //
  const target_type& lat (*rs.find_target_type ("liba"));
  const target_type& lst (*rs.find_target_type ("libs"));

  t.a = ma ? &search (t, lat, t.dir, t.out, t.name).as<liba> () : nullptr;
  t.s = ms ? &search (t, lst, t.dir, t.out, t.name).as<libs> () : nullptr;

  const target_type& oat (*rs.find_target_type ("obja"));
  const target_type& ost (*rs.find_target_type ("objs"));

  prerequisites aps, sps;

  // Iterate over the lib{} group's prerequisites. If we find an asm{} file,
  // we synthesize the corresponding static (obja) and shared (objs) objects.
  //
  for (prerequisite_member p: group_prerequisite_members (a, t))
  {
    if (include (a, t, p) != include_type::normal)
      continue;

    if (!p.is_a (at))
      continue;

    const target& as (p.search (t));

    // Name mangling again. See match() for the rationale.
    //
    string n (as.name + ".asm");

    if (t.a != nullptr)
    {
      // No need to check for collision with C/C++ objects. The name mangling
      // above should keep us safe.
      //
      pair<target&, ulock> l (
        search_new_locked (ctx, oat, t.dir, dir_path (), move (string (n))));

      if (l.second.owns_lock ())
      {
        l.first.prerequisites (prerequisites {prerequisite (as, true)});
        l.second.unlock ();
      }

      aps.push_back (prerequisite (l.first, true));
    }

    if (t.s != nullptr)
    {
      auto l (
        search_new_locked (ctx, ost, t.dir, dir_path (), move (string (n))));

      if (l.second.owns_lock ())
      {
        l.first.prerequisites (prerequisites {prerequisite (as, true)});
        l.second.unlock ();
      }

      sps.push_back (prerequisite (l.first, true));
    }
  }

  // Inject the synthesized objects into the members.
  //
  // The cc link rule (which will match the liba/libs members) will see these
  // alongside the objects it synthesizes from C/C++ sources.
  //
  if (t.a != nullptr && !aps.empty ())
    const_cast<target&> (static_cast<const target&> (*t.a))
      .prerequisites (move (aps));

  if (t.s != nullptr && !sps.empty ())
    const_cast<target&> (static_cast<const target&> (*t.s))
      .prerequisites (move (sps));

  // Delegate the actual work.
  //
  // We matched the 'lib' target, but we aren't going to link it ourselves.
  // Instead, we explicitly match the members (liba and/or libs). This hands
  // control over to the cc link rule, which matches liba/libs targets.
  //
  const target* m[] = {t.a, t.s};
  match_members (a, t, m);

  return &perform_lib;
}

static target_state
perform_lib (action a, const target& xt)
{
  const lib& t (xt.as<lib> ());
  const target* m[] = {t.a, t.s};
  return execute_members (a, t, m);
}

//   }; // class rule
// } // namespace build2
