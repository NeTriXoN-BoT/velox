# Corresponding source for the Linux release

This archive contains the exact upstream Qt/PySide archives and Debian source
packages for the shared system libraries included in the AppImage. The lock file
records their HTTPS origins, sizes and SHA-256 hashes. Debian `.dsc` files identify
each original archive and its Debian patches; `dpkg-source -x package.dsc` unpacks
the complete Debian source. Follow `debian/rules` and the package's build notes to
rebuild the corresponding shared library. Velox does not modify those libraries.

The AppImage runtime source is pinned to the upstream 20251108 release, including
its patches, Makefile and build scripts. Its LGPL libfuse 3.15.0 source and
squashfuse 0.5.2 source are included. Follow `BUILD.md` and
`scripts/common/install-dependencies.sh` in that runtime source to rebuild and
relink it with a modified libfuse. Use `appimagetool --runtime-file` to package the
replacement runtime with the extracted Velox AppDir. This does not require a
Velox signing key. The official runtime's other static libraries use permissive
licenses; their original notices are included in the AppImage.

The SDL and Wayland protocol archives used for Velox's native controls are also
included. Their build scripts and the application's complete modified source are
in the same Velox release tag:
https://github.com/NeTriXoN-BoT/velox

See `docs/LINUX-BUILD.md` there for the complete application build. Run the AppImage
with `--appimage-extract` to obtain its AppDir. Replace compatible shared libraries
inside that AppDir and run `AppRun`, or rebuild the AppImage. No application
signature check prevents replacing LGPL libraries. Original notices and license
texts are under `usr/share/licenses/velox` and the Python distributions' license
directories. Google SDK/system images are downloaded separately after acceptance
of Google's license and are not in this archive or the AppImage.
