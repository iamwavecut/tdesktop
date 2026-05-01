from pathlib import Path


path = Path("/lib/python3.12/site-packages/snapcraft/extensions/gnome.py")
text = path.read_text()
replacements = (
    (
        '''"source": str(source),
                    "plugin": "make",''',
        '''"source": str(source),
                    "source-type": "local",
                    "plugin": "make",''',
    ),
    (
        '''"source": str(source),
                "plugin": "make",''',
        '''"source": str(source),
                "source-type": "local",
                "plugin": "make",''',
    ),
)

patched = text
for old, new in replacements:
    patched = patched.replace(old, new)

if patched != text:
    path.write_text(patched)
    count = patched.count('"source-type": "local"') - text.count('"source-type": "local"')
    print(f"Patched Snapcraft GNOME extension source type in {count} location(s).")
elif '"source-type": "local"' in text:
    print("Snapcraft GNOME extension already has local source type.")
else:
    raise SystemExit("Could not patch Snapcraft GNOME extension source type.")

extension_data = Path("/usr/share/snapcraft/extensions/desktop")
package_data = Path("/usr/lib/python3.12/site-packages/extensions/desktop")

if not extension_data.exists():
    if not package_data.exists():
        raise SystemExit("Could not find Snapcraft desktop extension data.")
    extension_data.parent.mkdir(parents=True, exist_ok=True)
    extension_data.symlink_to(package_data, target_is_directory=True)
    print(f"Linked Snapcraft desktop extension data from {package_data}.")
