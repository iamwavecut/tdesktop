from pathlib import Path


path = Path("/lib/python3.12/site-packages/snapcraft/extensions/gnome.py")
text = path.read_text()
old = '''"source": str(source),
                    "plugin": "make",'''
new = '''"source": str(source),
                    "source-type": "local",
                    "plugin": "make",'''

if '"source-type": "local"' in text:
    print("Snapcraft GNOME extension already has local source type.")
elif old in text:
    patched = text.replace(old, new)
    path.write_text(patched)
    count = patched.count('"source-type": "local"') - text.count('"source-type": "local"')
    print(f"Patched Snapcraft GNOME extension source type in {count} location(s).")
else:
    raise SystemExit("Could not patch Snapcraft GNOME extension source type.")
