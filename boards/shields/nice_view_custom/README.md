# nice!view Custom — default left + atom "Y" right

- **Left (central)** — the **stock ZMK `nice!view` status screen** (default): battery,
  output/profile status, WPM graph and layer name. Restored unchanged from the
  vendored original in `/backup/nice_view_stock/`.
- **Right (peripheral)** — an animated **atom "Y"**: a nucleus with the letter "Y"
  and electrons orbiting on three tilted elliptical paths.

## Usage

In `build.yaml`, both halves use `shield: nice_view_custom`. The shield renders the
stock status screen on the central half and the atom animation on the peripheral.

## Options

- Invert the colors (dark background): `CONFIG_NICE_VIEW_WIDGET_INVERTED=y` in
  `nice_view_custom.conf`.
- Animation speed: the tick interval `K_MSEC(120)` in `widgets/peripheral_status.c`.

## Battery note

The peripheral animation redraws continuously, which costs more power than a static
screen. `CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE=y` blanks it when idle to limit the drain.

## Revert to the fully stock display (both halves)

The unmodified stock ZMK `nice_view` shield is vendored in `/backup/nice_view_stock/`.
To go back on both halves, switch `build.yaml` to `shield: nice_view`.

## Credits

Based on the stock ZMK `nice_view` shield (MIT), adapted for ZMK v0.3.0.
