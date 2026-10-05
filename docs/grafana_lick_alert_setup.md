# Grafana lick activity alerts to Slack

This guide configures alerts through Grafana's interface. It does not require changes to provisioning files.

## Behavior

- Evaluate every **30 seconds** using one database query for all rigs.
- A working rig has at least one record in `samples` within the last **5 minutes**. Weight and other sample types count; licks are stored separately in `events`.
- For each working rig, count **LICK events on channels 1 and 2 combined** during the last **1 minute**. With a one-minute window, this count is the rate in licks/minute.
- Start activity above **10 licks/minute** and reset below **2 licks/minute**. Between the thresholds, retain the previous state independently for each rig.
- Send an initial Slack notification and repeat while activity remains active, at your chosen **Repeat interval**.
- Automatically include newly working rigs and retire rigs that stop sending samples.

## 1. Slack app

The alert setup uses a **webhook URL**. If you already have the webhook, reuse it.

1. Open [Slack app management](https://api.slack.com/apps) in your browser. This is separate from the regular Slack chat interface.
2. Select your existing app, or click **Create New App → From scratch**, name it `Grafana Alerts`, and choose your Slack workspace.
3. Open **Incoming Webhooks** and enable **Activate Incoming Webhooks**.
4. Click **Add New Webhook to Workspace**, select the alerts channel, and click **Allow**.
5. Copy the URL beginning with `https://hooks.slack.com/services/`. Use it in step 2 below.

Source: [Grafana's Slack webhook setup](https://grafana.com/docs/grafana/latest/alerting/configure-notifications/manage-contact-points/integrations/configure-slack/).

## 2. Configure the Slack contact point

1. Open **Alerting → Contact points**.
2. Add a contact point named `slack-alerts`, or edit your existing one.
3. Select **Slack** as the integration.
4. Paste your Slack incoming webhook into **Webhook URL**.
5. Open **Optional settings** and replace the default title and text body with the following.

**Title:**

```gotemplate
Lick activity — Rig {{ .CommonLabels.rig_id }}
```

**Text body:**

```gotemplate
{{ range .Alerts.Firing }}
Combined lick rate: {{ printf "%.0f" (index .Values "B") }} licks/min.
{{ end }}
```

6. Enable **Disable resolved message**.
7. Click **Test → Send test notification**, then save.

The template deliberately uses value **B**, the lick rate defined in step 5. It does not include the rule's summary or description; those are not automatically appended to this custom body. The built-in test may omit the rig label and B value. Verify the final formatting with a real alert.

Expected message:

> **Lick activity — Rig 2**  
> Combined lick rate: 25 licks/min.

Source: [Grafana Slack contact point settings](https://grafana.com/docs/grafana/latest/alerting/configure-notifications/manage-contact-points/integrations/configure-slack/).

## 3. Create the notification policy

Open **Alerting → Notification policies**, and add a **New notification policy**.

| Setting | Value |
|---|---|
| Name | `notification` |
| Contact point | `slack-alerts` |
| Group by | `grafana_folder`, `alertname`, `rig_id` |
| Group wait | `0s` |
| Group interval | `30s` |
| Repeat interval | `10m` |

Save the policy. Choose a repeat interval that is a multiple of the group interval.

These group labels identify the folder, rule, and individual rig. Grouping by all three gives each rig a separate notification group. The query creates `rig_id`; the policy uses it.

Source: [Grafana notification policies](https://grafana.com/docs/grafana/latest/alerting/configure-notifications/create-notification-policy/).

## 4. Create alert rules
Open **Alerting → Alert rules → New alert rule**.

| Setting | Value |
|---|---|
| Rule name | `lick-rate` |

### Define query

Paste this query into **A**:

```sql
WITH working_rigs AS (
    SELECT DISTINCT rig_id
    FROM samples
    WHERE host_ts > now() - interval '5 minutes'
      AND host_ts <= now()
),
recent_licks AS (
    SELECT rig_id, count(*) AS lick_count
    FROM events
    WHERE type = 'LICK'
      AND channel IN (1, 2)
      AND host_ts > now() - interval '1 minute'
      AND host_ts <= now()
    GROUP BY rig_id
)
SELECT
    now() AS time,
    w.rig_id::text AS rig_id,
    COALESCE(l.lick_count, 0)::double precision AS lick_rate
FROM working_rigs w
LEFT JOIN recent_licks l ON l.rig_id = w.rig_id
ORDER BY time, w.rig_id;
```

Click **Run queries** or **Preview**. Expect one current value per working rig, including zero for a working rig with no licks. If no rigs have recent samples, the result is empty.

The windows use `host_ts`, the computer's receipt timestamp. They are fixed in the query and do not depend on the dashboard time picker or selected rig.

This version adds a `time` column to the earlier table query so it can use **Time series → Reduce B → Threshold C**, matching the Slack template. It does not change the counting logic. Keep `rig_id::text` so Grafana treats it as a label.

Add or edit the expressions to match this table. Their letters matter because the Slack body references **B**.

| Expression | Type | Input | Settings |
|---|---|---|---|
| B | Reduce | A | Function: **Last**; Mode: **Strict** |
| C | Threshold | A | **Is above 10** |

On **C**, enable **Custom recovery threshold** and set recovery to **Is below 2**. Click **Set as alert condition** on C. Use Threshold rather than Classic condition to keep rigs independent.

Source: [Grafana expressions and recovery thresholds](https://grafana.com/docs/grafana/latest/alerting/fundamentals/alert-rules/queries-conditions/).

### Folder

| Setting | Value |
| --- | --- |
| Folder | `hathaway-alert-system` |

## Evaluation schedule

Under **Configure alert evaluation behavior**, enter:

| Setting | Value |
|---|---|
| Evaluation group | Create or select `lick_activity_30s` |
| Evaluation interval | `30s` |
| Pending period | `0s` / None |
| Keep firing for | `0s` / None |
| No data | Normal |
| Error or timeout | Error |
| Missing series evaluations to resolve | `1`, if available |

`lick_activity_30s` is a group name. Its evaluation interval is the actual schedule; there are not two evaluation timers.

No data means no working rigs and no activity alert. Database errors remain distinguishable from inactivity. Error alerts can follow a separate notification route; this short Slack template is intended for lick activity, not database diagnostics.

A disconnected rig remains eligible until its last sample is older than five minutes. Once absent from a successful query, its alert instance is retired according to the missing-series setting. If all rigs disappear, the No data setting applies instead.

Source: [Grafana rule configuration](https://grafana.com/docs/grafana/latest/alerting/alerting-rules/create-grafana-managed-rule/).

### Save

Click **Save rule**.


## Timing reference

| Parameter | Value in this guide | Purpose | Where to change it |
|---|---|---|---|
| Working-rig window | `5 minutes` | How recently a rig must have sent a sample to remain monitored | Rule → Query A → `working_rigs`: change `interval '5 minutes'` |
| Lick-count window | `1 minute` | Rolling window used to calculate combined lick rate | Rule → Query A → `recent_licks`: change `interval '1 minute'`; also adjust the rate calculation as explained below |
| Evaluation interval | `30s` | Runs the database query twice per minute | Evaluation group `lick_activity_30s` → Edit → Evaluation interval |
| Pending period | `0s` / None | How long the activity condition must stay true before the alert fires | Rule → Configure alert evaluation behavior → Pending period |
| Keep firing for | `0s` / None | Extra time to stay active after the recovery condition is met | Rule → Configure alert evaluation behavior → Keep firing for |
| Missing series evaluations to resolve | `1` | Number of consecutive evaluations a previously tracked rig can be absent before being retired; this is a count, not seconds | Rule → Configure no data and error handling |
| Group wait | `0s` | Wait before the first notification for a new notification group | Notification policy → Timing options → Group wait |
| Group interval | `30s` | Paces notifications about changes to an existing notification group | Notification policy → Timing options → Group interval |
| Repeat interval | `10m` | Sends a reminder while the alert remains active | Notification policy → Timing options → Repeat interval |

### Open the relevant settings

1. **Query and rule settings:** Open **Alerting → Alert rules**, find `lick-rate`, and click its **Edit** button. Save the rule after changing it.
2. **Evaluation interval:** In the grouped alert-rule list, click **Edit** beside `lick_activity_30s`. Changing this interval affects every rule in that group. The group name is only a label; renaming it does not change the schedule.
3. **Notification timing:** Open **Alerting → Notification policies** (possibly under **Notification configuration**), find the policy routing this rule to `slack-alerts`, and select **Edit**. Enable **Override general timings** if needed, change the timing values, and save the policy.

### Changing the lick window without changing the units

The current query counts one minute of licks, so its count already equals licks/minute. For a window of **n minutes**, divide the count by **n** to keep the thresholds in licks/minute.

For example, to use a two-minute window, change the filter inside `recent_licks` to:

```sql
AND host_ts > now() - interval '2 minutes'
```

Then change the final rate expression to:

```sql
COALESCE(l.lick_count, 0)::double precision / 2.0 AS lick_rate
```

Leave the working-rig window unchanged unless you also want to change how long disconnected rigs remain eligible.

### How the timers interact

The five-minute and one-minute windows are lookback periods, not query schedules. Both are evaluated by the same query every 30 seconds. With no pending period, a threshold crossing is normally detected at the next evaluation, up to roughly 30 seconds later plus processing time.

The notification policy's 30-second **Group interval** is separate from the evaluation interval. **Repeat interval** controls reminders, not database queries; use a multiple of Group interval. An alert that resolves before Group wait expires may produce no notification.

A disconnected rig first ages out of the working-rig window. The missing-series count applies after it disappears from query results. If all rigs disappear, the rule's **No data = Normal** setting applies instead.

The notification system receives alert updates; it does not run another lick query. The rule continues evaluating while Grafana is running even when the browser is closed. Settings saved through the interface persist in the existing Grafana data volume.
