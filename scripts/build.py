#!/usr/bin/env python3
"""Build DQ9Recompiled end to end: venv, generated banks, then the runner.

Runs the two CMake projects from the README (the bank generator in build/ and
ndsrecomp's runner in build-runner/) with the per-platform flags each needs,
then copies the runner to the repo root as DQ9Recompiled[.exe].
"""
import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import zlib

from recomplauncher import stage_manifest

ROM_SHA1 = "ff761d349709f329c8ac4bd0023fdce21861e8a1"
PROJECT_NAME = "DQ9Recompiled"
BANK_BUILD_DIR = "build"
RUNNER_BUILD_DIR = "build-runner"
# The ROM the banks are built from, unpacked; dq9.nds is unpacked into it.
ROM_FILE = "dq9.nds"
ROM_DIR = os.path.join("assets", "YDQP")
RECOMP_NET_DIR = "recomp-net"
# Other releases whose players can run this build: a recipe rebuilds the
# Europe code from their ROM (ndsrecomp/tools/release_recipe.py) and is built
# into the executable. The recipes are in git (recipes/<id>.recipe) with
# their literal bytes (the few bytes of Europe code they carry) blanked; the
# build fills them in from ROM_DIR and checks the recipe against the banks,
# so only the Europe ROM is needed. With assets/<id>, the release unpacked
# next to ROM_DIR, the recipe in git is made again first.
# YDQJ's recipe applies to the Japanese ROM after its data plan
# (scripts/ja-convert.py, recipes/YDQJ.data, filled in like the recipes)
# has put its files in YDQP's layout with Japanese text.
RECIPE_RELEASES = ("YDQE", "YDQJ")
RECIPE_SOURCE_DIR = "recipes"
RECIPE_DIR = os.path.join("generated", "recipes")
JA_PLAN = os.path.join(RECIPE_DIR, "YDQJ.data")
RECIPE_OPTIONS = [
    "--prefix", "dq9",
    # Overlay 29's banks are built from its decrypted image
    # (tools/ov029_unprotect.py), not the bytes on the cartridge.
    "--skip-bank", "ov029",
    # The secure area holds SDK functions between random filler; the
    # functions are validated, the filler can be the other release's.
    "--filler", "arm9.bin:0x0-0x800",
]

# The self-updater checks UPDATE_REPO's latest GitHub release for an asset
# named ASSET_FORMAT ({tag} the release tag, {platform} platform_id()) plus
# .zip (Windows), .tar.gz (Linux), .dmg (macOS) or .apk (Android); the
# release workflow uploads them under these names.
UPDATE_REPO = "birabittoh/DQ9Recompiled"
ASSET_FORMAT = PROJECT_NAME + "-{tag}-{platform}"

# The runner always links the recompiled FreeBIOS banks, which are not part of
# DQ9_BANKS: ndsrecomp ships their configs and images, and nds_recompile (built
# with the banks above) turns them into ndsrecomp/generated/*.c.
FREEBIOS_BANKS = (
    ("freebios9", "drastic_bios_arm9.bin", "freebios_arm9"),
    ("freebios7", "drastic_bios_arm7.bin", "freebios_arm7"),
)

IS_WINDOWS = platform.system() == "Windows"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(args, check=True, **kwargs):
    print(f"+ {' '.join(str(a) for a in args)}", flush=True)
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0 and check:
        sys.exit(result.returncode)
    return result


def platform_id():
    """The {platform} of a release asset, as the updater computes it."""
    os_name = platform.system()
    if os_name == "Windows":
        os_id = "win"
    elif os_name == "Darwin":
        os_id = "mac"
    elif os_name == "Linux":
        os_id = "linux"
    else:
        die(f"unsupported OS: {os_name}")
    arch = platform.machine().lower()
    if arch in ("x86_64", "amd64"):
        arch_id = "amd64"
    elif arch in ("aarch64", "arm64"):
        arch_id = "arm64"
    else:
        die(f"unsupported architecture: {arch}")
    return f"{os_id}-{arch_id}"


def release_tag():
    """The release tag a build is stamped with: NDS_VERSION_NAME (set by the
    release workflow), else the newest v* tag, else "" (as the runner's
    CMake derives NDS_RELEASE_VERSION)."""
    tag = os.environ.get("NDS_VERSION_NAME", "")
    if not tag:
        result = subprocess.run(["git", "describe", "--tags", "--abbrev=0", "--match", "v*"],
                                capture_output=True, text=True)
        tag = result.stdout.strip() if result.returncode == 0 else ""
    if tag and not tag.startswith("v"):
        tag = "v" + tag
    return tag


def asset_name(tag):
    return ASSET_FORMAT.replace("{tag}", tag).replace("{platform}", platform_id())


def cmake_path(path):
    # CMake wants forward slashes, and semicolons separate list items.
    return os.path.abspath(path).replace("\\", "/")


def apply_recomp_net_patches():
    """Applies patches/recomp-net-*.patch to the recomp-net submodule.

    Changes we need from recomp-net that it does not have yet. A patch that
    is already in the tree (a rerun, or a recomp-net that has the change) is
    skipped, so this is safe on every build.
    """
    patch_dir = "patches"
    if not os.path.isdir(patch_dir):
        return
    for name in sorted(os.listdir(patch_dir)):
        if not (name.startswith("recomp-net-") and name.endswith(".patch")):
            continue
        patch = os.path.abspath(os.path.join(patch_dir, name))
        already = subprocess.run(
            ["git", "-C", RECOMP_NET_DIR, "apply", "--check", "--reverse", patch],
            capture_output=True).returncode == 0
        if already:
            continue
        applied = subprocess.run(
            ["git", "-C", RECOMP_NET_DIR, "apply", patch], capture_output=True)
        if applied.returncode != 0:
            die(f"{name} does not apply to {RECOMP_NET_DIR}/: "
                f"{applied.stderr.decode(errors='replace').strip()}")
        print(f"applied {name} to {RECOMP_NET_DIR}/")


def check_deps():
    missing = [dep for dep in ("cmake", "ninja") if shutil.which(dep) is None]
    if missing:
        die(f"missing required tool(s): {', '.join(missing)}")


def find_clang():
    """Return (clang, clang++), or None to leave CMake's default compiler."""
    # Versioned binaries (clang-20, clang++-20, ...) only exist on Linux.
    if not IS_WINDOWS:
        for version in range(30, 17, -1):
            cc, cxx = f"clang-{version}", f"clang++-{version}"
            if shutil.which(cc) and shutil.which(cxx):
                return cc, cxx
    if shutil.which("clang") and shutil.which("clang++"):
        return "clang", "clang++"
    if IS_WINDOWS:
        # The banks and runner are only built with LLVM clang (MSVC target) here.
        die("no clang/clang++ found in PATH (install LLVM, e.g. `scoop install llvm`)")
    return None


def venv_python():
    if IS_WINDOWS:
        return os.path.join(".venv", "Scripts", "python.exe")
    return os.path.join(".venv", "bin", "python3")


def ensure_venv():
    # The bank CMakeLists picks .venv's Python up by itself when it exists;
    # tools/prepare_dq9.py needs ndspy from requirements.txt.
    py = venv_python()
    if not os.path.exists(py):
        run([sys.executable, "-m", "venv", ".venv"])
    if run([py, "-c", "import ndspy, PIL"], check=False,
           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
        run([py, "-m", "pip", "install", "-r", "requirements.txt"])
    return py


def default_sdl3_dir():
    if os.environ.get("SDL3_DIR"):
        return os.environ["SDL3_DIR"]
    # On Windows there is no system SDL3; reuse EternalSonataReprise's static one.
    candidate = os.path.join("..", "EternalSonataReprise", "sdk", "cmake")
    if IS_WINDOWS and os.path.exists(os.path.join(candidate, "SDL3Config.cmake")):
        return candidate
    return None


def compiler_args(compilers):
    if compilers is None:
        return []
    cc, cxx = compilers
    return [f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]


def ensure_rom_folder(python):
    """The build reads the ROM unpacked (ndsrecomp/tools/nds_folder.py), from
    ROM_DIR, which is made from dq9.nds the first time."""
    if os.path.exists(os.path.join(ROM_DIR, "rom.toml")):
        return
    if not os.path.exists(ROM_FILE):
        die(f"neither {ROM_DIR}/ nor {ROM_FILE} found in the repo root: place "
            f"your Europe (YDQP) ROM there as {ROM_FILE}")
    run([python, "-I", os.path.join("ndsrecomp", "tools", "nds_folder.py"),
         "unpack", ROM_FILE, ROM_DIR])


def build_banks(args, compilers, python):
    configure = [
        "cmake", "-G", "Ninja", "-S", ".", "-B", BANK_BUILD_DIR,
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        f"-DDQ9_PYTHON={cmake_path(python)}",
        f"-DDQ9_ROM={cmake_path(os.path.abspath(ROM_DIR))}",
        f"-DNDSRECOMP_COMPILE_JOBS={args.jobs}",
        f"-DDQ9_HOOKS={'OFF' if args.no_hooks else 'ON'}",
        f"-DDQ9_FIRMWARE_BANKS={'ON' if args.firmware_banks else 'OFF'}",
    ] + compiler_args(compilers)
    run(configure)
    run(["cmake", "--build", BANK_BUILD_DIR, "--parallel", str(args.jobs),
         "--target", "dq9_romcheck", "dq9_generate_banks"])


def file_digest(paths):
    h = hashlib.sha1()
    for path in paths:
        with open(path, "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def make_ja_folder(python):
    """The Japanese release's data plan (scripts/ja-convert.py): made again
    from assets/YDQJ when that is unpacked and the plan's inputs changed,
    then filled in from ROM_DIR into JA_PLAN. Returns the folder
    YDQJ.recipe is made from (the plan applied to assets/YDQJ: YDQP's
    layout, YDQJ's code), or None when assets/YDQJ is not there."""
    tool = os.path.join("scripts", "ja-convert.py")
    source = os.path.join(RECIPE_SOURCE_DIR, "YDQJ.data")
    jp = os.path.join("assets", "YDQJ")
    work = os.path.join("generated", "ja")
    folder = os.path.join(work, "YDQJ")
    if os.path.exists(JA_PLAN):
        os.remove(JA_PLAN)
    if not os.path.exists(os.path.join(jp, "rom.toml")):
        if not os.path.exists(source):
            print(f"note: neither {jp}/ nor {source} found; the build will "
                  "not run the YDQJ release", flush=True)
            return None
        run([python, "-I", tool, "fill", source, "--pal", ROM_DIR, JA_PLAN])
        return None
    os.makedirs(work, exist_ok=True)
    inputs = [os.path.join("scripts", n) for n in (
        "ja-convert.py", "ja_text.py", "ja-map.py", "ja-files.toml",
        "l5_archive.py", "dq9_text_table.py")] + [
        os.path.join(jp, "rom.toml"), os.path.join(ROM_DIR, "rom.toml")]
    stamp = os.path.join(work, "plan.stamp")
    digest = file_digest(inputs)
    current = os.path.exists(stamp) and open(stamp).read() == digest
    if not (current and os.path.exists(source)
            and os.path.exists(os.path.join(folder, "rom.toml"))):
        if os.path.exists(stamp):
            os.remove(stamp)
        run([python, "-I", tool, "plan", "--pal", ROM_DIR, "--jp", jp,
             "--cache", work, source])
        run([python, "-I", tool, "fill", source, "--pal", ROM_DIR, JA_PLAN])
        if os.path.exists(folder):
            shutil.rmtree(folder)
        run([python, "-I", tool, "apply", JA_PLAN, "--jp", jp, folder])
        with open(stamp, "w") as f:
            f.write(digest)
    else:
        run([python, "-I", tool, "fill", source, "--pal", ROM_DIR, JA_PLAN])
    return folder


def write_ja_plan_inc():
    """src/hooks/ja_plan.generated.inc: the filled Japanese data plan,
    deflated, for src/hooks/ja_convert.cpp (empty without one). Rewritten
    only when it changes."""
    out = os.path.join("src", "hooks", "ja_plan.generated.inc")
    lines = ["// scripts/build.py: the filled scripts/ja-convert.py plan, deflated.\n"]
    size = 0
    if os.path.exists(JA_PLAN):
        with open(JA_PLAN, "rb") as f:
            raw = f.read()
        size = len(raw)
        packed = zlib.compress(raw, 9)
        lines.append(f"#define DQ9_JA_PLAN_SIZE {size}u\n")
        lines.append("static const unsigned char kJaPlan[] = {\n")
        for i in range(0, len(packed), 32):
            lines.append(",".join(str(b) for b in packed[i:i + 32]) + ",\n")
        lines.append("};\n")
    else:
        lines.append("#define DQ9_JA_PLAN_SIZE 0u\n")
        lines.append("static const unsigned char kJaPlan[] = {0};\n")
    text = "".join(lines)
    if os.path.exists(out):
        with open(out, encoding="utf8") as f:
            if f.read() == text:
                return
    with open(out, "w", encoding="utf8", newline="\n") as f:
        f.write(text)
    print(f"{out}: {size} bytes of plan", flush=True)


def make_recipes(python):
    """Fill in each RECIPE_RELEASES recipe for the build, making it again
    first when its release is unpacked."""
    tool = os.path.join("ndsrecomp", "tools", "release_recipe.py")
    recomp = os.path.join("generated", "recomp")
    os.makedirs(RECIPE_DIR, exist_ok=True)
    ja_folder = make_ja_folder(python)
    for release in RECIPE_RELEASES:
        source = os.path.join(RECIPE_SOURCE_DIR, f"{release}.recipe")
        out = os.path.join(RECIPE_DIR, f"{release}.recipe")
        made = os.path.join(RECIPE_DIR, f"{release}.made.recipe")
        for stale in (out, made):
            if os.path.exists(stale):
                os.remove(stale)
        # YDQJ's recipe applies to its data plan's output, not to the ROM
        # as it is unpacked.
        folder = ja_folder if release == "YDQJ" else os.path.join("assets", release)
        if folder and os.path.exists(os.path.join(folder, "rom.toml")):
            run([python, "-I", tool, "make", ROM_DIR, folder, recomp, made]
                + RECIPE_OPTIONS)
            run([python, "-I", tool, "blank", made, source])
        elif not os.path.exists(source):
            print(f"note: neither {folder}/ nor {source} found; the build "
                  f"will not run the {release} release", flush=True)
            continue
        if run([python, "-I", tool, "fill", source, ROM_DIR, recomp, out],
               check=False).returncode != 0:
            die(f"{source} no longer fits the banks: make it again by "
                f"building with the {release} release unpacked in {folder}/, "
                f"and commit it")
        if os.path.exists(made):
            with open(made, "rb") as a, open(out, "rb") as b:
                if a.read() != b.read():
                    die(f"{out}: filling {source} did not give back the "
                        f"recipe made from {folder}/")
            os.remove(made)


def recipe_files():
    return [os.path.abspath(os.path.join(RECIPE_DIR, f"{release}.recipe"))
            for release in RECIPE_RELEASES
            if os.path.exists(os.path.join(RECIPE_DIR, f"{release}.recipe"))]


def ensure_freebios_banks():
    """Recompile the FreeBIOS images the runner links, if they are missing."""
    recompiler = os.path.join(BANK_BUILD_DIR, "ndsrecomp-recompiler",
                              f"nds_recompile{EXE_SUFFIX}")
    out = os.path.join("ndsrecomp", "generated")
    for config, image, bank in FREEBIOS_BANKS:
        if all(os.path.exists(os.path.join(out, f"{bank}{suffix}"))
               for suffix in (".c", "_dispatch.c", ".h")):
            continue
        run([
            recompiler,
            "--config", os.path.join("ndsrecomp", "bios", f"{config}.toml"),
            "--bin", os.path.join("ndsrecomp", "third_party", "freebios", image),
            "--out", out, "--bank", bank,
        ])


def build_runner(args, compilers):
    configure = [
        "cmake", "-G", "Ninja", "-S", os.path.join("ndsrecomp", "runner"),
        "-B", RUNNER_BUILD_DIR,
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
        "-DNDS_BOOTSTRAP_FIRMWARE=ON",
        f"-DNDS_TITLE_BANK_DIR={cmake_path(os.path.join('generated', 'recomp'))}",
        f"-DNDS_TITLE_ROM_SHA1={ROM_SHA1}",
        f"-DNDS_TITLE_CONFIG={cmake_path('game.toml')}",
        f"-DNDS_RELEASE_RECIPES={';'.join(cmake_path(r) for r in recipe_files())}",
        f"-DNDS_INTERPRETER={'ON' if args.interpreter else 'OFF'}",
        f"-DNDSRECOMP_COMPILE_JOBS={args.jobs}",
        # game.toml boots on FreeBIOS with a generated firmware, so the retail
        # BIOS banks (recompiled from the user's dumps) are never used, and
        # without them neither the build nor the game needs any dump.
        "-DNDS_RETAIL_BIOS_BANKS=OFF",
        "-DNDS_EMBED_DUMPS_DIR=",
        # The self-updater (ndsrecomp/runner/src/update/self_update.h).
        f"-DNDS_UPDATE_REPO={UPDATE_REPO}",
        f"-DNDS_UPDATE_ASSET_FORMAT={ASSET_FORMAT}",
        f"-DNDS_RELEASE_VERSION_OVERRIDE={args.version or ''}",
    ] + compiler_args(compilers)

    # Hooks are baked into the banks unless --no-hooks, and then the runner
    # must link their handlers (a missing one fails the link).
    if not args.no_hooks:
        # The hooks' own text (res/lang/*.toml -> src/hooks/lang.generated.h).
        run([sys.executable, os.path.join("scripts", "gen-lang.py")])
        # English place names for the Discord activity, from the ROM.
        run([sys.executable, os.path.join("scripts", "gen-map-names.py")])
        hooks_dir = os.path.join("src", "hooks")
        hooks = sorted(cmake_path(os.path.join(hooks_dir, f))
                       for f in os.listdir(hooks_dir) if f.endswith(".cpp"))
        configure.append(f"-DNDS_TITLE_HOOK_SOURCES={';'.join(hooks)}")
    else:
        configure.append("-DNDS_TITLE_HOOK_SOURCES=")

    sdl3_dir = args.sdl3_dir or default_sdl3_dir()
    if sdl3_dir:
        configure.append(f"-DSDL3_DIR={cmake_path(sdl3_dir)}")
    elif IS_WINDOWS:
        die("no SDL3 found: pass --sdl3-dir <dir with SDL3Config.cmake> or set SDL3_DIR")

    # The cartridge banner icon becomes the Windows .exe icon.
    rom = os.path.abspath(ROM_DIR)
    if os.path.exists(os.path.join(rom, "rom.toml")):
        configure.append(f"-DNDS_EXE_ICON_ROM={cmake_path(rom)}")

    # Netplay is on unless --no-online; recomp-net comes from its submodule
    # unless --recomp-net points elsewhere.
    configure.append(f"-DNDS_ONLINE={'ON' if args.online else 'OFF'}")
    if args.online:
        recomp_net = args.recomp_net or RECOMP_NET_DIR
        configure.append(f"-DNDS_RECOMP_NET_ROOT={cmake_path(recomp_net)}")

    if IS_WINDOWS:
        configure += [
            # C++ only, so the generated C banks are unaffected.
            "-DCMAKE_CXX_FLAGS=-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -D__WIN32__",
            "-DCMAKE_EXE_LINKER_FLAGS=-ld3d12 -ldxgi",
        ]

    run(configure)
    run(["cmake", "--build", RUNNER_BUILD_DIR, "--parallel", str(args.jobs),
         "--target", "nds_runner"])

    src = os.path.join(RUNNER_BUILD_DIR, f"nds_runner{EXE_SUFFIX}")
    dst = f"{PROJECT_NAME}{EXE_SUFFIX}"
    print(f"+ cp {src} {dst}")
    try:
        shutil.copy2(src, dst)
    except PermissionError:
        die(f"cannot overwrite {dst}: close any running copy and run again")


def do_package(name):
    import tarfile
    import zipfile

    if not name:
        tag = release_tag()
        if not tag:
            die("no release tag to name the package after: pass --package NAME, "
                "set NDS_VERSION_NAME or create a v* tag")
        name = asset_name(tag)

    exe = f"{PROJECT_NAME}{EXE_SUFFIX}"
    if not os.path.isfile(exe):
        die(f"{exe} not found: build first")

    # macOS ships an .app in a .dmg, which the updater swaps whole.
    if platform.system() == "Darwin":
        run([sys.executable, os.path.join("scripts", "package-mac.py"), "--name", name,
             "--version", release_tag().lstrip("v") or "0.0.0"])
        return

    # game.toml is embedded in the executable and it needs no dumps, so it
    # ships on its own with the licenses. pkg/ holds the same files loose.
    files = [exe, "LICENSE", "LICENSE-GPL", "README.md"]
    pkg_dir = "pkg"
    if os.path.isdir(pkg_dir):
        shutil.rmtree(pkg_dir)
    os.makedirs(pkg_dir)
    for f in files:
        print(f"+ cp {f} {pkg_dir}/")
        shutil.copy2(f, pkg_dir)
    # RecompLauncher reads it next to the executable.
    stage_manifest(pkg_dir, IS_WINDOWS)
    files.append("recomplauncher.json")

    if IS_WINDOWS:
        archive_path = f"{name}.zip"
        print(f"+ zip {archive_path}")
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in files:
                zf.write(os.path.join(pkg_dir, f), f)
    else:
        archive_path = f"{name}.tar.gz"
        print(f"+ tar {archive_path}")
        with tarfile.open(archive_path, "w:gz") as tf:
            for f in files:
                tf.add(os.path.join(pkg_dir, f), arcname=f)

    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with open(github_env, "a") as fh:
            fh.write(f"ARTIFACT_PATH={archive_path}\n")
            fh.write(f"ARTIFACT_NAME={name}\n")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--package", metavar="NAME", nargs="?", const="",
                   help="Package the built executable into NAME.zip (Windows), NAME.tar.gz "
                        "(Linux) or pkg/NAME.dmg (macOS) and skip the build; without NAME, "
                        f"the release asset name ({ASSET_FORMAT}) for NDS_VERSION_NAME or "
                        "the newest v* tag")
    p.add_argument("--version", help="Release version the self-updater compares against "
                                     "(X.Y.Z); default: NDS_VERSION_NAME, else the newest v* tag, "
                                     "else none, which turns the updater off")
    p.add_argument("--release", dest="build_type", action="store_const",
                   const="Release", default="RelWithDebInfo",
                   help="Build an optimized release without debug symbols; default is RelWithDebInfo")
    p.add_argument("--jobs", "-j", type=int, default=min(os.cpu_count() or 1, 6),
                   help="Parallel compile jobs (default: min(CPUs, 6); the bank C is ~1 GB, "
                        "so keep it low on hosts with little RAM)")
    p.add_argument("--runner-only", action="store_true",
                   help="Skip the bank build and only rebuild the runner")
    p.add_argument("--banks-only", action="store_true",
                   help="Only generate the banks (and the FreeBIOS banks), e.g. before "
                        "building the runner with a cross toolchain")
    p.add_argument("--no-hooks", action="store_true",
                   help="Generate banks without config/hooks and link no hook handlers")
    p.add_argument("--firmware-banks", action="store_true",
                   help="Also build the firmware boot banks (DQ9_FIRMWARE_BANKS=ON)")
    p.add_argument("--interpreter", action="store_true",
                   help="Keep the interpreter in the runner as a fallback")
    p.add_argument("--sdl3-dir", help="Directory with SDL3Config.cmake (default: $SDL3_DIR, "
                                      "or ../EternalSonataReprise/sdk/cmake on Windows)")
    p.add_argument("--online", action=argparse.BooleanOptionalAction, default=True,
                   help="Build Netplay (online lobbies, NDS_ONLINE=ON; default); "
                        "--no-online builds the local-wireless-only runner")
    p.add_argument("--recomp-net", help="Path to a recomp-net checkout to use instead "
                                        f"of the {RECOMP_NET_DIR}/ submodule")
    return p.parse_args()


def main():
    args = parse_args()

    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    os.chdir(root)

    if args.package is not None:
        do_package(args.package)
        return

    submodules = [
        os.path.join("ndsrecomp", "runner", "CMakeLists.txt"),
    ]
    if args.online and not args.recomp_net:
        submodules.append(os.path.join(RECOMP_NET_DIR, "include", "recomp_net", "auth.h"))
    if not all(os.path.exists(path) for path in submodules):
        die("submodules missing: run `git submodule update --init --recursive`")

    if args.online and not args.recomp_net:
        apply_recomp_net_patches()

    check_deps()
    compilers = find_clang()

    if not args.runner_only:
        python = ensure_venv()
        ensure_rom_folder(python)
        build_banks(args, compilers, python)
        make_recipes(python)
        ensure_freebios_banks()
        if args.banks_only:
            # The hooks' text, which the runner build would generate.
            run([sys.executable, os.path.join("scripts", "gen-lang.py")])
            run([sys.executable, os.path.join("scripts", "gen-map-names.py")])
            return
    elif not os.path.isdir(os.path.join("generated", "recomp")):
        die("generated/recomp missing: run without --runner-only first")

    write_ja_plan_inc()
    build_runner(args, compilers)


if __name__ == "__main__":
    main()
