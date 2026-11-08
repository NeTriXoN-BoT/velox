"""Build Velox's Android apps with SDK tools and a private signing key; no Gradle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]


def run(arguments):
    subprocess.run(list(map(str, arguments)), check=True)


def build(options):
    sdk, java = options.sdk.resolve(strict=True), options.java_home.resolve(strict=True) / "bin"
    tools = sdk / "build-tools/35.0.0"
    library = sdk / "platforms/android-35/android.jar"
    executable = ".exe" if os.name == "nt" else ""
    javac, jvm = java / ("javac" + executable), java / ("java" + executable)
    d8, signer = tools / "lib/d8.jar", tools / "lib/apksigner.jar"
    for path in (javac, jvm, library, d8, signer, options.keystore, options.password_file):
        if not path.is_file() or path.is_symlink():
            raise SystemExit("Provide regular SDK, JDK and private signing files: " + str(path))
    output = options.out.absolute()
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    manifests = {}
    for name in ("helper", "companion"):
        source = ROOT / name
        with tempfile.TemporaryDirectory(prefix=".android-", dir=output) as temporary:
            work = Path(temporary)
            if not work.resolve().is_relative_to(output.resolve()):
                raise RuntimeError("Android build staging escaped its output folder.")
            classes, dex = work / "classes", work / "dex"
            classes.mkdir()
            dex.mkdir()
            sources = sorted((source / "src").rglob("*.java"))
            version = "1.0"
            if name == "companion":
                digest = hashlib.sha256()
                for file in sorted(path for path in source.rglob("*") if path.is_file()):
                    digest.update(file.read_bytes())
                version = digest.hexdigest()[:24]
                generated = work / "Build.java"
                generated.write_text(
                    'package app.velox.home; final class Build { static final String ID = "'
                    + version
                    + '"; }\n',
                    encoding="utf-8",
                )
                sources.append(generated)
            run(
                [
                    javac,
                    "--release",
                    "11",
                    "-encoding",
                    "UTF-8",
                    "-classpath",
                    library,
                    "-d",
                    classes,
                    *sources,
                ]
            )
            archive = work / "classes.jar"
            with zipfile.ZipFile(archive, "w") as target:
                for file in sorted(classes.rglob("*.class")):
                    target.write(file, file.relative_to(classes).as_posix())
            run(
                [
                    jvm,
                    "-cp",
                    d8,
                    "com.android.tools.r8.D8",
                    "--release",
                    "--min-api",
                    "30",
                    "--lib",
                    library,
                    "--output",
                    dex,
                    archive,
                ]
            )
            aapt, aligned = tools / ("aapt2" + executable), work / "aligned.apk"
            resources = []
            if (source / "res").is_dir():
                compiled = work / "resources.zip"
                run([aapt, "compile", "--dir", source / "res", "-o", compiled])
                resources.append(compiled)
            base = work / "base.apk"
            run(
                [
                    aapt,
                    "link",
                    "-o",
                    base,
                    "-I",
                    library,
                    "--manifest",
                    source / "AndroidManifest.xml",
                    "--min-sdk-version",
                    "30",
                    "--target-sdk-version",
                    "35",
                    "--version-name",
                    version,
                    *resources,
                ]
            )
            with zipfile.ZipFile(base, "a") as target:
                target.write(dex / "classes.dex", "classes.dex")
            run([tools / ("zipalign" + executable), "-p", "-f", "4", base, aligned])
            result = work / (name + ".apk")
            run(
                [
                    jvm,
                    "-jar",
                    signer,
                    "sign",
                    "--ks",
                    options.keystore.resolve(),
                    "--ks-key-alias",
                    options.alias,
                    "--ks-pass",
                    "file:" + str(options.password_file.resolve()),
                    "--out",
                    result,
                    aligned,
                ]
            )
            run([jvm, "-jar", signer, "verify", "--verbose", result])
            manifests[name + ".apk"] = hashlib.sha256(result.read_bytes()).hexdigest()
            os.replace(result, output / result.name)
    (output / "android-build.json").write_text(
        json.dumps(manifests, indent=2, sort_keys=True) + "\n"
    )
    print(
        "Built and signature-verified both Android apps. Private keys remain outside the package."
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--keystore", type=Path, required=True)
    parser.add_argument("--password-file", type=Path, required=True)
    parser.add_argument("--alias", default="helper")
    parser.add_argument("--out", type=Path, required=True)
    build(parser.parse_args())
