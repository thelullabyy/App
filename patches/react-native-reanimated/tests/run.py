"""Run the native CSS exit regression with real React Native nodes, Yoga and JavaScriptCore on macOS."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fast-float-include", type=Path, required=True)
    parser.add_argument("--dependency-prefix", type=Path, default=Path("/opt/homebrew"))
    options = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This host test uses the macOS JavaScriptCore framework")
    if not (options.fast_float_include / "fast_float/fast_float.h").exists():
        parser.error("--fast-float-include must contain fast_float/fast_float.h (React Native pins v8.0.0)")

    app = Path(__file__).resolve().parents[3]
    rn = app / "node_modules/react-native/ReactCommon"
    rea = app / "node_modules/react-native-reanimated/Common/cpp"
    build = Path(tempfile.mkdtemp(prefix="reanimated-exiting-css-"))
    schema = build / "schema.json"
    generated = build / "generated"
    subprocess.run([
        "node", str(app / "node_modules/@react-native/codegen/lib/cli/combine/combine-js-to-schema-cli.js"),
        str(schema), str(app / "node_modules/react-native-reanimated/src/specs"),
    ], check=True)
    subprocess.run([
        "node", str(app / "node_modules/react-native/scripts/generate-specs-cli.js"),
        "--platform", "ios", "--schemaPath", str(schema), "--outputDir", str(generated),
        "--libraryName", "rnreanimated", "--libraryType", "components",
    ], check=True)

    includes = [options.dependency_prefix / "include", options.fast_float_include, rn, rea,
                app / "node_modules/react-native-worklets/Common/cpp", generated]
    includes += [path for path in rn.iterdir() if path.is_dir()]
    includes += list(rn.glob("react/**/platform/cxx"))
    includes += list(rn.glob("react/nativemodule/*"))
    flags = ["clang++", "-std=c++20", "-O0", "-g0", "-DGLOG_USE_GLOG_EXPORT",
             "-ffunction-sections", "-fdata-sections", "-ferror-limit=3"]
    flags += [f"-I{path}" for path in includes]

    parts = ["react/renderer/core", "react/renderer/components/view", "react/renderer/components/root",
             "react/renderer/graphics", "react/renderer/debug", "react/renderer/mounting",
             "react/renderer/componentregistry", "react/renderer/css", "react/utils", "react/debug",
             "react/featureflags", "react/renderer/telemetry", "jsi/jsi", "yoga/yoga",
             "react/renderer/components/scrollview"]
    sources = []
    for part in parts:
        sources += [path for path in (rn / part).rglob("*.cpp")
                    if not {"test", "tests", "android", "ios"}.intersection(path.parts)
                    and ("platform" not in path.parts or "cxx" in path.parts)]
    sources += [rea / "reanimated" / path for path in [
        "Fabric/updates/UpdatesRegistryManager.cpp", "Fabric/updates/UpdatesRegistry.cpp",
        "Fabric/ShadowTreeCloner.cpp", "CSS/registries/StaticPropsRegistry.cpp",
        "CSS/misc/ViewStylesRepository.cpp", "LayoutAnimations/LayoutAnimationsProxyCommon.cpp",
        "LayoutAnimations/PropsDiffer.cpp",
    ]]
    sources += [rn / "jsc/JSCRuntime.cpp", rn / "react/renderer/dom/DOM.cpp",
                Path(__file__).with_name("RetainedCSSAnimationsTest.cpp")]

    def compile_source(source):
        target = build / (hashlib.sha256(str(source).encode()).hexdigest()[:16] + ".o")
        result = subprocess.run(flags + ["-c", str(source), "-o", str(target)], capture_output=True, text=True)
        if result.returncode:
            print(result.stderr, file=sys.stderr, flush=True)
            return None
        return str(target)

    with ThreadPoolExecutor(max_workers=6) as pool:
        objects = list(pool.map(compile_source, sources))
    if any(path is None for path in objects):
        return 1
    binary = build / "regression"
    # Unused platform services have no host implementation. The exercised renderer,
    # layout, registry and mounting code is compiled above and runs against real nodes.
    subprocess.run([
        "clang++", *objects, f"-L{options.dependency_prefix / 'lib'}", "-lfolly", "-lfmt", "-lglog",
        "-framework", "JavaScriptCore", "-Wl,-dead_strip", "-Wl,-undefined,dynamic_lookup", "-o", str(binary),
    ], check=True)
    print(f"Native regression binary: {binary}", flush=True)
    return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    sys.exit(main())
