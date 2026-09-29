# Shared figure style

Read this reference whenever a plot or figure is requested. These colors and
symbol shapes are shared across plot types when the corresponding quantities
are shown. User instructions override these defaults. Reuse the visual meaning;
do not add an element that is irrelevant to the requested figure.

## Color palette

| Meaning | Color | Hex |
|---|---|---|
| Spout 1 | Dark blue | `#134E6F` |
| Spout 2 | Coral | `#FF6150` |
| HIT | Pale cyan | `#D5EDF8` |
| INCORRECT | Pale red | `#F6D9D9` |
| Early lick = 1, either HIT or INCORRECT | Muted purple | `#B0A6BA` |
| Phase onset and go cue | Black | `#000000` |

When a single color encodes a combined outcome/early-lick class, use purple for
Early lick = 1 and the outcome color for Early lick = 0. Unknown early-lick
status must not be silently treated as 0. No colors are defined for TEACH,
NO_RESPONSE, ABORT, or unknown classes; do not silently reuse another class's
color. Keep the plot background white.

## Symbol shapes

- Individual licks: filled circles without outlines, colored by spout.
- Go cue: a filled black triangle pointing right when represented by a symbol.
- Phase-onset lines: black and solid when such lines are shown.

These rules define appearance, not marker size, event alignment, triangle
geometry, class-bar placement, panel arrangement, or output format.

## Plot-specific layout routing

Only when the user's request explicitly includes **"raster plot"**
(case-insensitive, including "lick raster plot" or "raster plots"), or continues
an already requested raster plot, read and apply [raster-plot.md](raster-plot.md).
It defines alignment, time windows, marker sizes and physical proportions,
panel layout, output formats, and raster-specific checks.

Do not load or apply those layout rules for a generic scatter plot, figure, or
lick-data request. Other plot types use their requested layout and outputs;
they have no prescribed layout in these references yet.

## Independent plotting workflow

Implement acquisition and trial preparation within the new task using
[database.md](database.md), [tasks-and-analyses.md](tasks-and-analyses.md), and
[telemetry.md](telemetry.md), as relevant to the analysis. A plot must not require
saved data or code from a different analysis task. Fetch missing inputs from
PostgreSQL read-only; allow explicit refresh of the task's own cached inputs.
Document dependencies and commands in the task's scripts. Verify a clean run
without cached inputs and reuse of the task's own snapshot.

Generate all current figures through the task's `analysis.py`; expose plotting
and analysis controls there even when rendering uses helper modules. Deliver
the saved database inputs and `report.md` with the pipeline, figure definitions,
settings, output inventory, and reproduction commands. Keep the report current
when changing a figure or its selection.
