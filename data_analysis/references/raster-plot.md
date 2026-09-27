# Raster plot layout

Read and apply this reference only when the user's request explicitly includes
"raster plot" (case-insensitive, including "lick raster plot" or "raster plots"),
or continues an already requested raster plot. A generic scatter plot, figure,
or lick-data request does not activate these rules. User instructions override
these defaults.

Use the shared colors and symbol shapes in [figure-making.md](figure-making.md).
The alignment, dimensions, panel arrangement, and outputs below are specific to
raster plots and must not become defaults for other plot types.

## Data and selection

- Reconstruct trials and indicators using [tasks-and-analyses.md](tasks-and-analyses.md)
  and interpret telemetry using [telemetry.md](telemetry.md).
- One row represents one trial. Plot recorded LICK events, with separate
  colors for spouts 1 and 2. Use device timestamps and sequence numbers.
- Apply the requested session, sample, outcome, early-lick, and head-fixing
  filters before selecting trials. For random selection, sample without
  replacement with a recorded seed, then order selected trials chronologically.
- Keep the eligible count, selected trial list, seed, filters, and exclusions
  with the analysis. Do not silently balance head-fixing categories or duplicate
  trials to reach a requested count. Report if too few trials are available.
- Missing boundaries or alignment events must be handled explicitly. A trial
  without a response onset cannot receive a go-cue-to-response triangle.

## Alignment and time window

Default to sample onset at time zero. The first delay onset will also align
when sample durations are equal; otherwise show its actual time per row.
Keep real elapsed time through every delay replay and early-lick pause.

Use a common 0–6 second window for comparable sample-aligned panels unless
the user requests another range. Clip display only, not the saved trial data;
report when response onsets fall outside the displayed window. Do not omit
long trials just to fit the figure.

Draw sample and initial delay onset as **black solid lines**. Use a full
vertical line when a boundary is shared by all rows; otherwise use a short
segment for each row at its actual boundary. Do not mark delay replays as
new sample phases.

When response alignment is explicitly requested, use response onset as zero
and a black vertical line there instead of the go-cue triangles. Keep each
trial's actual earlier phase times.

## Marker emphasis and panel layout

The first recorded lick after response onset, across either spout, gets a larger
marker in that spout's color. Resolve equal timestamps using sequence order.
Do not enlarge the first lick of the trial or a lick before response onset.

Place a solid class-color bar at the far left, outside the time axis, spanning
the panel's trial rows. Use the shared palette: early-lick purple takes priority
over outcome color when the class combines both. No class-bar color is defined
for other outcomes or unknown early-lick status; do not silently map them to
these classes.

Keep panels separate by default. Stack class panels vertically only when
requested; group composition and trial counts come from the user's request.

## Go-cue triangle and physical proportions

For sample-aligned rasters, use a filled black triangle pointing right:

- Its vertical **left side is exactly at go-cue onset**.
- Its **right tip is exactly at response-window onset**.
- Let `B` be the rendered length of the vertical left side.
- Horizontal distance from left side to right tip: **1.2 × B**.
- Row-center to row-center distance: **2 × B**.
- First-response-lick marker diameter: **1 × B**.

The earlier 1.25× first-lick diameter and 1.5× row spacing were superseded.
Ordinary lick markers should be smaller; the approved 6-second example uses
approximately **5 points** in diameter (versus 9 points for the first lick).

Compute these ratios in display units, not by equating seconds and row units.
For an axis of width `A` points spanning `T` seconds and a go cue lasting `g`
seconds: triangle width `W = A*g/T`, `B = W/1.2`, row spacing `2*B`, and large
marker diameter `B`. Adjust the plotting area's height to get the required
row spacing. In Matplotlib scatter, set `s = diameter_in_points**2` with no
marker outline. Use a polygon with time-based vertices for the triangle;
a fixed-size scatter triangle will not necessarily match both event times.

Do not distort recorded cue durations to force these ratios. If cue durations
vary, flag the conflict with uniform row spacing and choose a layout explicitly
rather than silently using a nominal duration.

## Output and checks

- Label time in seconds and identify the session and trial filters. State
  the selected count and head-fixing composition. Show a spout/marker legend.
- Verify marker proportions after layout, selected trial counts, alignment,
  and the first-lick selection. Inspect the rendered figure for clipping and
  overlapping labels.
- Save runnable plotting code and selection information in the analysis task
  folder unless the user requests previews only. Provide PNG previews; when
  requested for PowerPoint editing, export SVG with vector marks and editable
  text (`svg.fonttype = 'none'` in Matplotlib).

## Preparing raster inputs

For lick rasters, retain raw STATE and LICK records with `t_us` and `seq`, and
a trial table containing session, rig, task, chronological trial number, sample,
outcome, start/end timestamps and sequence numbers, head-fixing status, and
early-lick status. Retain parameter information and the acquisition quality audit.
Save excluded fragments and reasons separately from eligible trials.

Slice each trial's records from its start through its end in `(t_us, seq)` order.
Locate the first DELAY entry, GOCUE entry, and RESPONSE entry. Subtract sample
onset (or the explicitly requested alignment event) and divide microseconds by
1,000,000 to get elapsed seconds. Identify the first response lick using the first
LICK on spout 1 or 2 strictly after RESPONSE in `(t_us, seq)` order. Preserve delay
replays and pauses. Validate required boundaries before random selection; report
missing or ambiguous phases as exclusions rather than silently using substitutes.

Apply the user's filters and trial counts, record the random seed, and render
using the geometry above. Verify a clean run without cached inputs, as well as
reuse of the task's own snapshot. Session IDs, sample groupings, counts, and seeds
are task choices, not defaults inherited from an earlier figure.
