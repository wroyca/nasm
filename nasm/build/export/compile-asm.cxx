// c++ 1 //---

//---

// namespace build2
// {
//   class rule: public simple_rule
//   {

bool
match (action a, target& t) const
{
  tracer trace ("nasm.compile::match");

  const scope& rs (*t.base_scope ().root_scope ());

  // We need to determine if this rule is applicable to the target. The logic is
  // straightforward: we look at the prerequisites. If we see an asm{} target,
  // we claim it.
  //
  const target_type* asm_tt (rs.find_target_type ("asm"));

  if (asm_tt == nullptr)
    return false;

  // Iterate over the prerequisites group. Note that we only care about the
  // immediate prerequisites that are included in the build (exclude
  // ad-hoc/order-only ones if necessary, though include() handles that).
  //
  for (prerequisite_member p: group_prerequisite_members (a, t))
  {
    if (include (a, t, p) != include_type::normal)
      continue;

    if (p.is_a (*asm_tt))
      return true;
  }

  l4 ([&]{trace << "no asm{} prerequisite for target " << t;});
  return false;
}

virtual recipe
apply (action a, target& xt, match_extra& me) const override
{
  file& t (xt.as<file> ());

  const scope& rs (*t.base_scope ().root_scope ());

  // Look up the target platform system since we need it for deriving the
  // object file extension.
  //
  const string& tsys (cast<string> (rs["cc.target.system"]));

  // Derive the object file path with a compound extension that encodes the link
  // output type. This mirrors the cc compile rule convention which prevents
  // obja{} and objs{} with the same name from colliding.
  //
  // The convention (derived from cc/compile-rule.cxx):
  //
  //   target   win32-msvc   mingw32   darwin     other (ELF)
  //   ------   ----------   -------   ------     -----------
  //   obje{}   exe.obj      exe.o     .o         .o
  //   obja{}   lib.obj      a.o       a.o        a.o
  //   objs{}   dll.obj      dll.o     dylib.o    so.o
  //
  {
    const char* o (tsys == "win32-msvc" ? "obj" : "o");

    string e;

    const string& tn (t.type ().name);

    if (tsys == "win32-msvc")
    {
      if      (tn == "obje") e = "exe.";
      else if (tn == "obja") e = "lib.";
      else                   e = "dll.";
    }
    else if (tsys == "mingw32")
    {
      if      (tn == "obje") e = "exe.";
      else if (tn == "obja") e = "a.";
      else                   e = "dll.";
    }
    else if (tsys == "darwin")
    {
      if      (tn == "obje") e = "";
      else if (tn == "obja") e = "a.";
      else                   e = "dylib.";
    }
    else // Linux, BSD, etc.
    {
      if      (tn == "obje") e = "";
      else if (tn == "obja") e = "a.";
      else                   e = "so.";
    }

    e += o;

    t.derive_path (e.c_str ());
  }

  // Inject the output directory dependency (but don't match yet, we'll
  // batch-match everything at the end).
  //
  inject_fsdir (a, t, false /* match */);

  // Search and add the target's user-declared prerequisites (e.g., the asm{}
  // source file specified in the buildfile).
  //
  // The pattern's asm{} prerequisite is a regex pattern (not a substitution),
  // so apply_prerequisites() will skip it (the user-declared prerequisite is
  // the one that actually matters).
  //
  search_prerequisite_members (a, t);

  // Inject any non-pattern prerequisites from the pattern rule (e.g., literal
  // tool prerequisites). Note that pattern prerequisites are skipped since they
  // were already handled above.
  //
  pattern->apply_prerequisites (a, t, t.base_scope (), me);

  // Match all the prerequisite members.
  //
  match_members (a, t, t.prerequisite_targets[a]);

  switch (a)
  {
    case perform_update_id: return perform_update;
    case perform_clean_id:  return perform_clean_depdb;
    default:                return noop_recipe;
  }
}

// Determine NASM output format.
//
// Note that NASM's format names (elf64, win64, macho64) don't map 1:1 to our
// standard target triplets, so we have to do some translation.
//
// Also note that for Mach-O (macOS/iOS), we need to distinguish between
// static and shared object generation if we are on 64-bit, though usually
// this is handled by the linker.
//
struct nasm_format
{
  const char* name;
};

static nasm_format
output_format (const string& tc,
               const string& cpu,
               bool          shared)
{
  bool x64 (cpu == "x86_64" || cpu == "amd64");

  if      (tc == "windows")              return {x64 ? "win64"   : "win32"};
  else if (tc == "macos" || tc == "ios") return {x64 ? "macho64" : "macho32"};
  else                                   return {x64 ? "elf64"   : "elf32"};
}

static target_state
perform_update (action a, const target& xt)
{
  tracer trace ("nasm.compile::perform_update");

  context& ctx (xt.ctx);

  const scope& bs (xt.base_scope ());
  const scope& rs (*bs.root_scope ());

  const file& t (xt.as<file> ()); // obje{}, obja{}, or objs{}.
  const path& tp (t.path ());

  // Determine whether we are building for a shared library. We look up
  // the target type by name since the bin module's types (objs, obja, obje)
  // are not available as C++ classes in ad-hoc recipe context.
  //
  bool shared (t.type ().name == "objs");

  // Look up target platform information.
  //
  const string& tclass (cast<string> (rs["cc.target.class"]));
  const string& cpu    (cast<string> (rs["cc.target.cpu"]));

  nasm_format fmt (output_format (tclass, cpu, shared));

  // Find asm{} source and NASM executable among the prerequisites.
  //
  // We don't (can't) store these directly in the target as they are part of the
  // prerequisite graph, so we must iterate the prerequisites to find them.
  //
  const target_type* asm_tt (rs.find_target_type ("asm"));
  assert (asm_tt != nullptr);

  const target_type* exe_tt (rs.find_target_type ("exe"));
  assert (exe_tt != nullptr);

  timestamp mt (t.load_mtime ());
  auto& pts (t.prerequisite_targets[a]);

  // The asm{} prerequisite.
  //
  const file& s (find_if (pts.begin (),
                          pts.end (),
                          [asm_tt] (const prerequisite_target& pt)
  {
    return pt.target != nullptr && pt.target->type ().is_a (*asm_tt);
  })->target->as<file> ());

  // The exe{nasm} prerequisite.
  //
  const file& nasm_exe (
    find_if (pts.begin (),
             pts.end (),
             [exe_tt] (const prerequisite_target& pt)
  {
    return pt.target != nullptr && pt.target->type ().is_a (*exe_tt);
  })->target->as<file> ());

  process_path nasm_pp (run_search (nasm_exe.path ()));

  // Check if any prerequisites are newer than the target.
  //
  optional<target_state> ps (execute_prerequisites (a, t, mt));

  // Build the command line. We hash everything into the depdb to detect
  // option changes even when file timestamps haven't changed.
  //
  cstrings args {nasm_pp.recall_string ()};

  args.push_back ("-f");
  args.push_back (fmt.name);

  append_options (args, t, "nasm.poptions");
  append_options (args, t, "nasm.coptions");

  args.push_back ("-o");
  args.push_back (tp.string ().c_str ());
  args.push_back (s.path ().string ().c_str ());

  depdb dd (tp + ".d");
  {
    xxh64 cs;
    for (const char* a: args)
      cs.append (a);

    if (dd.expect (cs.string ()) != nullptr)
      l4 ([&]{trace << "command line mismatch forcing update of " << t;});
  }

  // If the command line changed or the database is new, force a rebuild.
  //
  if (dd.writing () || dd.mtime > mt)
    ps = nullopt;

  dd.close ();

  // If everything is up to date, we are done.
  //
  if (ps)
    return *ps;

  args.push_back (nullptr);

  if (verb == 1)
    print_diag ("nasm", s, t);
  else if (verb >= 2)
    print_process (args);

  // Execute the command.
  //
  // Note that we don't currently parse NASM-generated dependency files (e.g.,
  // -MD). If we ever add support for assembly files including other files (via
  // %include), we will need to add that logic here.
  //
  if (!ctx.dry_run)
  {
    run (ctx, nasm_pp, args, 1 /* finish_verbosity */);
    dd.check_mtime (tp);
  }

  t.mtime (system_clock::now ());
  return target_state::changed;
}

//   }; // class rule
// } // namespace build2
