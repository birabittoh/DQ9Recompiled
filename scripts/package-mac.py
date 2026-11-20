#!/usr/bin/env python3
"""Packages the built macOS executable as an .app inside a .dmg.

Run after build.py, which leaves DQ9Recompiled in the repo root; this only
assembles. `build.py --package` calls it on macOS.

The self-updater downloads the .dmg, copies the .app out of it with ditto and
swaps the installed bundle whole, which keeps its code signature valid (see
ndsrecomp/runner/src/update/self_update_mac.cpp). Libraries the executable
links from outside the system (SDL3 from Homebrew, say) are copied into
Contents/Frameworks and relinked there, so the bundle runs on a machine
without them.

Ad-hoc signed at the end, because arm64 will not run unsigned code and
relinking a library invalidates its signature. Not notarised, so a first
launch needs right-click -> Open.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

PROJECT_NAME = "DQ9Recompiled"
BUNDLE_ID = "com.birabittoh.dq9recompiled"
DISPLAY_NAME = "DQ9Recompiled"
# The oldest macOS the runner is built for (CMake's default deployment target
# on the CI runners).
MINIMUM_SYSTEM_VERSION = "13.0"
# Inside the bundle, not beside it in the image: the updater copies every
# top-level entry of the image next to the installed .app, which is usually
# /Applications.
FILES_IN_RESOURCES = ["LICENSE", "LICENSE-GPL", "README.md"]
# The unpacked ROM build.py configures from (its NDS_EXE_ICON_ROM), whose
# banner icon becomes the app's.
ROM_DIR = os.path.join("assets", "YDQP")
ICON_FILE = "AppIcon.icns"


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def linked_libraries(binary):
    """Non-system dylibs `binary` links, as the install names it records."""
    out = run(["otool", "-L", binary], capture_output=True, text=True).stdout
    libs = []
    for line in out.splitlines()[1:]:
        name = line.strip().split(" (")[0]
        if not name or name.startswith(("/usr/lib/", "/System/", "@")):
            continue
        libs.append(name)
    return libs


def bundle_libraries(exe, frameworks_dir):
    """Copies the executable's non-system dylibs (and theirs) into
    Contents/Frameworks and points every reference at the copies."""
    os.makedirs(frameworks_dir, exist_ok=True)
    pending = [exe]
    done = set()
    while pending:
        binary = pending.pop()
        for name in linked_libraries(binary):
            base = os.path.basename(name)
            target = os.path.join(frameworks_dir, base)
            if base not in done:
                done.add(base)
                print(f"+ cp {name} Contents/Frameworks/")
                # copy2 follows symlinks, so a versioned Homebrew link lands
                # as one real file under the name it is linked by.
                shutil.copy2(name, target)
                os.chmod(target, 0o755)
                run(["install_name_tool", "-id", f"@rpath/{base}", target])
                pending.append(target)
            run(["install_name_tool", "-change", name, f"@rpath/{base}", binary])
    if done:
        run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", exe])


def write_icon(resources_dir):
    """The cartridge banner icon as Resources/<ICON_FILE>, for Finder, the
    Dock and the system dialogs (the running game's window icon only reaches
    the Dock once its window is open). False without the unpacked ROM."""
    rom = os.path.join(ROOT, ROM_DIR)
    if not os.path.isfile(os.path.join(rom, "rom.toml")):
        print(f"warning: {ROM_DIR} not found, the app gets no icon",
              file=sys.stderr)
        return False
    run([sys.executable, "-I",
         os.path.join(ROOT, "ndsrecomp", "tools", "gen_nds_icon.py"),
         "--icns", rom, os.path.join(resources_dir, ICON_FILE)])
    return True


def write_info_plist(contents_dir, version, has_icon):
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": DISPLAY_NAME,
        "CFBundleExecutable": PROJECT_NAME,
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": DISPLAY_NAME,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSApplicationCategoryType": "public.app-category.games",
        "LSMinimumSystemVersion": MINIMUM_SYSTEM_VERSION,
        # Without this the window server upscales the window, which on
        # Retina is visibly soft.
        "NSHighResolutionCapable": True,
    }
    if has_icon:
        info["CFBundleIconFile"] = ICON_FILE
    path = os.path.join(contents_dir, "Info.plist")
    print(f"+ write {path}")
    with open(path, "wb") as fh:
        plistlib.dump(info, fh)


def build_app(app_path, version):
    if os.path.exists(app_path):
        shutil.rmtree(app_path)
    contents = os.path.join(app_path, "Contents")
    macos_dir = os.path.join(contents, "MacOS")
    resources = os.path.join(contents, "Resources")
    os.makedirs(macos_dir)
    os.makedirs(resources)

    src = os.path.join(ROOT, PROJECT_NAME)
    if not os.path.isfile(src):
        print(f"error: {PROJECT_NAME} not found in {ROOT}; run build.py first",
              file=sys.stderr)
        sys.exit(1)
    exe = os.path.join(macos_dir, PROJECT_NAME)
    print(f"+ cp {PROJECT_NAME} Contents/MacOS/")
    shutil.copy2(src, exe)
    os.chmod(exe, 0o755)

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from recomplauncher import stage_manifest
    stage_manifest(macos_dir, is_windows=False, is_macos=True)

    for name in FILES_IN_RESOURCES:
        shutil.copy2(os.path.join(ROOT, name), resources)
    has_icon = write_icon(resources)
    bundle_libraries(exe, os.path.join(contents, "Frameworks"))
    write_info_plist(contents, version, has_icon)

    # --deep is deprecated for real identities but right for an ad-hoc pass
    # over relinked dylibs. Fatal on failure: on arm64 the result would not
    # run.
    run(["codesign", "--force", "--deep", "--sign", "-", app_path])


def build_dmg(app_path, dmg_path, volume_name):
    staging = os.path.join(ROOT, "pkg-dmg")
    if os.path.exists(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)

    shutil.copytree(app_path, os.path.join(staging, os.path.basename(app_path)),
                    symlinks=True)
    # The drag-to-install link, and why a dmg beats a zip: an app dragged out
    # of an image is freed from App Translocation.
    os.symlink("/Applications", os.path.join(staging, "Applications"))

    if os.path.exists(dmg_path):
        os.remove(dmg_path)
    run(["hdiutil", "create", "-volname", volume_name, "-srcfolder", staging,
         "-ov", "-format", "UDZO", dmg_path])
    shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True,
                        help="Base name for the .dmg, without the extension")
    parser.add_argument("--version", default="0.0.0",
                        help="CFBundleShortVersionString / CFBundleVersion")
    args = parser.parse_args()

    if sys.platform != "darwin":
        print("error: this only runs on macOS (needs codesign, hdiutil, otool)",
              file=sys.stderr)
        sys.exit(1)

    os.chdir(ROOT)
    app_path = os.path.join(ROOT, f"{DISPLAY_NAME}.app")
    build_app(app_path, args.version)

    # Into pkg/, which is what CI uploads.
    pkg_dir = os.path.join(ROOT, "pkg")
    os.makedirs(pkg_dir, exist_ok=True)
    dmg_path = os.path.join(pkg_dir, f"{args.name}.dmg")
    build_dmg(app_path, dmg_path, DISPLAY_NAME)
    print(f"packaged {dmg_path}")

    github_env = os.environ.get("GITHUB_ENV")
    if github_env:
        with open(github_env, "a") as fh:
            fh.write(f"ARTIFACT_PATH={os.path.relpath(dmg_path, ROOT)}\n")
            fh.write(f"ARTIFACT_NAME={args.name}\n")


if __name__ == "__main__":
    main()
