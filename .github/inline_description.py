import sys

vdf_path = sys.argv[1]
desc_path = sys.argv[2]

with open(desc_path) as f:
    desc = f.read().strip()

# Escape for VDF string value
desc = desc.replace('\\', '\\\\').replace('"', '\\"')

with open(vdf_path) as f:
    vdf = f.read()

vdf = vdf.replace('DESCRIPTION_FILE_PLACEHOLDER', desc)

with open(vdf_path, 'w') as f:
    f.write(vdf)
