"""A port's Android app (port.py package --android; docs/porting.md,
"Android"): an APK for arm64 phones with Vulkan 1.1, into dist/:

    dist/<Name>-<version>-android-arm64.apk

The runtime is built for it with the NDK (in --android-build, the runtime's
build-android/ by default): libmain.so, the launcher and the player in one
program SDL's activity runs (frontend/android_main.cpp), with the port's own
UI, and 3dsrecomp, which the launcher runs on the phone. Zig for AArch64
Linux (its own build, checked against its SHA-256) compiles the game there:
its program as libzig.so, the part of its lib folder that compiling without
a C library takes as an asset. The Gradle project (the runtime's android/,
copied into the port's build/android/) packs them with SDL's Java, the
port's files (port.toml, the scenes, the hot lists, each game's native code)
and its icon (icon.png beside [package] icon).

Signed with Android's debug key, unless ANDROID_KEYSTORE (with
ANDROID_KEYSTORE_PASSWORD, ANDROID_KEY_ALIAS, ANDROID_KEY_PASSWORD) names
the port's own. The SDK comes from ANDROID_HOME (or ANDROID_SDK_ROOT, or
where Android Studio puts it), the NDK from ANDROID_NDK_HOME or the SDK's
newest. Nothing of a game goes in, as in the package for computers.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

from portfile import PortError

SDK = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(SDK)

# Zig for the phone, from ziglang.org/download/index.json
ZIG_NAME = "zig-aarch64-linux-0.17.0"
ZIG_URL = "https://ziglang.org/download/0.17.0/zig-aarch64-linux-0.17.0.tar.xz"
ZIG_SHA256 = "9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8"
# what of its lib folder compiling C for aarch64-linux-musl without a C
# library takes: clang's headers, musl's and Linux's, and what Zig builds
# its own support from; the files at the folder's top too
ZIG_KEEP = ("include/", "std/", "compiler_rt/", "compiler/", "libc/include/generic-musl/",
            "libc/include/aarch64-linux-musl/", "libc/include/any-linux-any/", "libc/musl/")


def sdk_root():
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(name) and os.path.isdir(os.environ[name]):
            return os.environ[name]
    for guess in (os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"),
                  os.path.expanduser("~/Android/Sdk"), os.path.expanduser("~/Library/Android/sdk")):
        if os.path.isdir(guess):
            return guess
    raise PortError("no Android SDK: set ANDROID_HOME (Android Studio's, or the command-line tools')")


def ndk_root(sdk):
    for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if os.environ.get(name) and os.path.isdir(os.environ[name]):
            return os.environ[name]
    ndks = os.path.join(sdk, "ndk")
    versions = sorted(os.listdir(ndks), key=lambda v: [int(x) for x in re.findall(r"\d+", v)]) if os.path.isdir(ndks) else []
    if not versions:
        raise PortError(f"no NDK in {ndks}: sdkmanager --install \"ndk;<version>\", or set ANDROID_NDK_HOME")
    return os.path.join(ndks, versions[-1])


def host_tag():
    system = {"win32": "windows", "darwin": "darwin"}.get(sys.platform, "linux")
    return f"{system}-x86_64"


def run(command, say, cwd=None, env=None):
    say("> " + " ".join(f'"{c}"' if " " in c else c for c in command))
    if subprocess.run(command, cwd=cwd, env=env).returncode:
        raise PortError(f"{os.path.basename(command[0])} failed")


def build_runtime(port, args, ndk, say):
    """libmain.so and 3dsrecomp for arm64, with the port's UI; their paths
    and SDL's Java sources"""
    build = os.path.abspath(args.android_build or os.path.join(RUNTIME, "build-android"))
    exe = ".exe" if os.name == "nt" else ""
    glslc = os.path.join(ndk, "shader-tools", host_tag(), "glslc" + exe)
    ui = port.ui.replace("\\", "/") if port.ui else ""
    run(["cmake", "-S", RUNTIME, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ndk, "build", "cmake", "android.toolchain.cmake").replace("\\", "/"),
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29", "-DANDROID_STL=c++_static",
         "-DRUNTIME_VULKAN=ON", "-DVulkan_GLSLC_EXECUTABLE=" + glslc.replace("\\", "/"), f"-DRUNTIME_PORT_UI={ui}",
         "-DRUNTIME_PORT_ICON="], say)
    run(["cmake", "--build", build, "--target", "main", "3dsrecomp"], say)
    java = os.path.join(build, "_deps", "sdl3-src", "android-project", "app", "src", "main", "java")
    for path in (os.path.join(build, "libmain.so"), os.path.join(build, "3dsrecomp"), java):
        if not os.path.exists(path):
            raise PortError(f"no {path} after building the runtime for Android")
    return os.path.join(build, "libmain.so"), os.path.join(build, "3dsrecomp"), java, build


def zig_for_phone(cache, say):
    """Zig's program for AArch64 Linux, and its lib folder's part as a tar
    (names relative to lib/), downloaded and cut once into cache"""
    program, lib_tar = os.path.join(cache, "libzig.so"), os.path.join(cache, "lib.tar")
    if os.path.exists(program) and os.path.exists(lib_tar):
        return program, lib_tar
    os.makedirs(cache, exist_ok=True)
    archive = os.path.join(cache, ZIG_NAME + ".tar.xz")
    if not os.path.exists(archive) or hashlib.sha256(open(archive, "rb").read()).hexdigest() != ZIG_SHA256:
        say(f"downloading {ZIG_URL}")
        with urllib.request.urlopen(ZIG_URL) as response, open(archive + ".part", "wb") as out:
            shutil.copyfileobj(response, out)
        os.replace(archive + ".part", archive)
        if hashlib.sha256(open(archive, "rb").read()).hexdigest() != ZIG_SHA256:
            os.remove(archive)
            raise PortError("the Zig downloaded is not the one expected (its SHA-256 differs)")
    say(f"cutting {ZIG_NAME}'s lib folder down to what the phone needs")
    prefix = ZIG_NAME + "/lib/"
    with tarfile.open(archive, "r:xz") as source, tarfile.open(lib_tar + ".part", "w", format=tarfile.GNU_FORMAT) as out:
        for member in source:
            if member.name == ZIG_NAME + "/zig" and member.isfile():
                with open(program + ".part", "wb") as f:
                    shutil.copyfileobj(source.extractfile(member), f)
                continue
            if not member.name.startswith(prefix) or not member.isfile():
                continue
            name = member.name[len(prefix):]
            if "/" in name and not name.startswith(ZIG_KEEP):
                continue
            member.name = name
            out.addfile(member, source.extractfile(member))
    if not os.path.exists(program + ".part"):
        raise PortError(f"no zig program in {archive}")
    os.replace(program + ".part", program)
    os.replace(lib_tar + ".part", lib_tar)
    return program, lib_tar


def version_code(version):
    parts = [int(x) for x in re.findall(r"\d+", version)[:3]] + [0, 0, 0]
    return max(1, parts[0] * 10000 + parts[1] * 100 + parts[2])


def package_android(port, args, say, build_native):
    """build_native(port, args, game): port.py's native, the game's code's folder"""
    from package import copy, third_party

    sdk = sdk_root()
    ndk = ndk_root(sdk)
    say(f"Android SDK {sdk}, NDK {ndk}")
    libmain, recomp, sdl_java, build = build_runtime(port, args, ndk, say)
    zig, zig_lib = zig_for_phone(os.path.join(build, "zig"), say)
    icon = os.path.join(os.path.dirname(port.icon), "icon.png") if port.icon else ""
    if not icon or not os.path.exists(icon):
        raise PortError("the app needs the port's icon: port.toml's [package] icon, with icon.png beside it")

    project = os.path.join(port.root, "build", "android")
    shutil.rmtree(project, ignore_errors=True)
    shutil.copytree(os.path.join(RUNTIME, "android"), project)
    copy(sdl_java, os.path.join(project, "app", "src", "main", "java"))
    copy(icon, os.path.join(project, "res", "drawable-nodpi", "icon.png"))
    from xml.sax.saxutils import escape
    os.makedirs(os.path.join(project, "res", "values"))
    with open(os.path.join(project, "res", "values", "strings.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                f'    <string name="app_name">{escape(port.name)}</string>\n</resources>\n')

    # the programs, stripped: the launcher and the player, 3dsrecomp, Zig
    libs = os.path.join(project, "jniLibs", "arm64-v8a")
    os.makedirs(libs)
    strip = os.path.join(ndk, "toolchains", "llvm", "prebuilt", host_tag(), "bin", "llvm-strip" + (".exe" if os.name == "nt" else ""))
    for source, name in ((libmain, "libmain.so"), (recomp, "lib3dsrecomp.so")):
        run([strip, "--strip-unneeded", "-o", os.path.join(libs, name), source], say)
    shutil.copy2(zig, os.path.join(libs, "libzig.so"))

    # the port's files, as the launcher has them beside it elsewhere
    assets = os.path.join(project, "assets")
    files = os.path.join(assets, "port")
    copy(port.file, os.path.join(files, "port.toml"))
    if os.path.exists(os.path.join(port.root, "single-screen.toml")):
        copy(os.path.join(port.root, "single-screen.toml"), os.path.join(files, "single-screen.toml"))
    if os.path.isdir(os.path.join(port.root, "hot")):
        copy(os.path.join(port.root, "hot"), os.path.join(files, "hot"))
    if port.native_sources():
        import argparse
        for game in port.games:
            if not game.decomp:
                continue
            build_args = argparse.Namespace(command="package", picked=game, rom=None, update=None, unpacked=None,
                                            extra=[], includes=[], out=None, no_symbols=False,
                                            runtime_build=getattr(args, "runtime_build", None))
            try:
                folder = build_native(port, build_args, game)
            except PortError as e:
                if game.native_from:
                    say(f"! {game.label()} goes without the port's code: {e}")
                    continue
                raise
            for name in ("decomp.bin", "decomp.txt"):
                copy(os.path.join(folder, name), os.path.join(files, game.decomp, name))
    licenses = os.path.join(files, "LICENSES")
    copy(os.path.join(RUNTIME, "LICENSE"), os.path.join(licenses, "Bergamota.txt"))
    copy(os.path.join(RUNTIME, "LICENSE-zakuro"), os.path.join(licenses, "Zakuro.txt"))
    copy(os.path.join(RUNTIME, "LICENSE-3dsrecomp"), os.path.join(licenses, "3dsrecomp.txt"))
    copy(os.path.join(RUNTIME, "launcher", "assets", "Nunito-OFL.txt"), os.path.join(licenses, "Nunito-OFL.txt"))
    with open(os.path.join(licenses, "THIRD_PARTY.md"), "w", encoding="utf-8") as f:
        f.write(third_party(port, build) + "\n- **Zig** (" + ZIG_NAME + ", which builds the game on the phone), MIT; "
                "its lib folder's parts under their own licenses (musl: MIT; LLVM's headers: Apache-2.0 with LLVM's exception).\n")
    names, digest = [], hashlib.sha256()
    for root, _, found in os.walk(files):
        for name in sorted(found):
            path = os.path.join(root, name)
            relative = os.path.relpath(path, files).replace("\\", "/")
            names.append(relative)
            digest.update(relative.encode() + b"\0" + open(path, "rb").read())
    with open(os.path.join(files, "files.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(names)) + "\n")
    with open(os.path.join(files, "stamp"), "w") as f:
        f.write(digest.hexdigest())
    copy(zig_lib, os.path.join(assets, "zig", "lib.tar"))
    with open(os.path.join(assets, "zig", "stamp"), "w") as f:
        f.write(ZIG_NAME + " " + hashlib.sha256("\n".join(ZIG_KEEP).encode()).hexdigest())

    # the app's name, id, version and signing
    app_id = port.android_id or "org.bergamota." + re.sub(r"[^a-z0-9_]", "", port.name.lower())
    props = {"id": app_id, "name": port.name, "version": args.version, "version_code": str(version_code(args.version))}
    if os.environ.get("ANDROID_KEYSTORE"):
        props.update({"keystore": os.path.abspath(os.environ["ANDROID_KEYSTORE"]).replace("\\", "/"),
                      "keystore_password": os.environ.get("ANDROID_KEYSTORE_PASSWORD", ""),
                      "key_alias": os.environ.get("ANDROID_KEY_ALIAS", ""),
                      "key_password": os.environ.get("ANDROID_KEY_PASSWORD", "")})
    with open(os.path.join(project, "port.properties"), "w", encoding="utf-8") as f:
        for k, v in props.items():
            f.write(f"{k}={v.replace(chr(92), chr(92) * 2)}\n")
    with open(os.path.join(project, "local.properties"), "w", encoding="utf-8") as f:
        f.write("sdk.dir=" + sdk.replace("\\", "/") + "\n")

    gradle = os.path.join(project, "gradlew.bat" if os.name == "nt" else "gradlew")
    if os.name != "nt":
        os.chmod(gradle, 0o755)
    run([gradle, "--no-daemon", "assembleRelease"], say, cwd=project)
    apk = os.path.join(project, "app", "build", "outputs", "apk", "release", "app-release.apk")
    if not os.path.exists(apk):
        raise PortError(f"Gradle made no {apk}")
    out = os.path.join(port.root, "dist", f"{port.name}-{args.version}-android-arm64.apk")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copy2(apk, out)
    say(f"{out}: {os.path.getsize(out) / 2**20:.1f} MB ({app_id})")
