<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | cpu | gpu | storage | audio | battery | wifi | network | thermal | usb |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [Galaxy Note FE](gracerlte.md) | `exynos8890` | yes | yes | yes | partly | yes | yes |  | yes | yes |  | yes | yes |
| [QEMU virt (aarch64)](qemu.md) | `none` | yes | yes | yes |  | no | yes | yes |  |  | yes |  |  |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `exynos8890` | Samsung Exynos 8890 (Exynos 8 Octa) | carried by gracerlte (`devices/gracerlte/soc/`) |
