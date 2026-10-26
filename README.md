# Oplus Device Battery Health Reader

> Language: English | [简体中文](README_CN.md)

A **Magisk / KernelSU / APatch** module that reads and displays the battery health result (diagnostic item `030103`) directly on the phone.

The goal is to behave consistently with OPPO's official after-sales diagnostic tool (O+ Support): read the battery data the device already provides, grade it the same way, and honestly report "unsupported" when the data is unavailable — instead of guessing or substituting approximations.

> The result is highly informative, but it is affected by device model, system version and firmware differences. **Always treat the official after-sales tool as the final authority.**

## Features

* Reads the battery health percentage and grade (Excellent / Good / Poor), with the grade ranges explained the same way as the official tool
* Three ways to use it: terminal output, a WebUI page, and the module "action" button
* Optional extra telemetry (level, voltage, current, temperature, charge state, cycle count) — these values **never** influence the health result
* Uses only the device's own official data sources; when data is missing it clearly reports "unsupported", with no compatibility fallback and no substitute source

## Requirements

* A rooted device (Magisk v24+ / KernelSU / APatch)
* Android 10 or newer
* A shell-capable environment (any regular terminal app works)

## Installation

Flash the module ZIP in Magisk / KernelSU / APatch and reboot. The install script performs the necessary checks and setup.

## Usage

### 1. Command line

```bash
su -c batteryhealth                 # human-readable output
su -c "batteryhealth --kv"          # key=value form (for scripting)
su -c "batteryhealth --json"        # single-line JSON
su -c "batteryhealth --kv --extra"  # add extra telemetry
```

### 2. WebUI

* **KernelSU / APatch** — open the module and tap *WebUI*.
* **Magisk** — Magisk has no built-in WebUI host, so the module starts its own local HTTP server at boot (built-in implementation, no busybox dependency) listening on `127.0.0.1:8899`. Open `http://127.0.0.1:8899/` in any browser.

  To disable that server:

  ```bash
  su -c "touch /data/adb/modules/oppo_battery_health/disable_http"
  ```

  Third-party hosts such as KsuWebUIStandalone or MMRL work as well.

### 3. Action button

The module ships `action.sh`, so Magisk / KernelSU / APatch show an *Action* button. Tapping it opens the module page; the page only performs a read when you press *Read battery health*.

> The action button requires the screen to be on — a sleeping device cannot bring the browser to the foreground. Using the action button naturally satisfies this.

## Sample output

```
电池健康 (030103)
  percent      = 76.65 %
  level        = 良
  legend       = 优：80-100%，良：60-80%，差：0-60%
  status       = normal

  fcc          = 3550 mAh
  design       = 4510 mAh
  判级与记录（等级用未舍入值判定）:
    ratio_exact= 76.651884700699995
    record_int = 76
```

Field reference:

| Field | Meaning |
|---|---|
| `percent` | Battery health percentage (two decimals, display only) |
| `level` | Grade: 优 / 良 / 差 (Excellent / Good / Poor), matching the official grading |
| `legend` | Percentage range explained for each grade |
| `status` | Decision status, see the table below |
| `fcc` | Current full-charge capacity |
| `design` | Factory design capacity |
| `ratio_exact` | Exact ratio value used for grading (not rounded) |
| `record_int` | Truncated value, matching what the official tool records |

Decision status:

| status | Meaning |
|---|---|
| `normal` | Data available, grade is Excellent or Good |
| `abnormal` | Health value is below the official lower threshold (grade: Poor) |
| `unsupported` | The required data is not available on this device/firmware |

Because grading uses the unrounded value, a result such as `59.999 %` — displayed as `60.00 %` — is still graded as *Poor*. This matches the official tool's behaviour.

## Design principles

* **Official data only.** Everything used comes from battery information already present in the device firmware; no third-party database and no estimation model.
* **No compatibility fallback.** If any piece of data required by the official grading is missing, the module reports "unsupported" instead of falling back to a result of unclear origin.
* **Display is separated from grading.** The two decimals on screen are formatting only; grading always uses the exact value, so rounding can never push a value across a threshold.
* **Telemetry does not affect grading.** Voltage, current, temperature and cycle count are shown separately as reference data only.

## Side effects and privacy

* The module **only prints the result and writes no files** — no result files, no system configuration changes.
* **No notifications** (the notification feature of earlier versions was removed because most systems silently drop shell-posted notifications).
* Nothing is uploaded to the network: all reading happens locally on the device, and the WebUI server listens on the loopback address only.
* At boot the module starts a local page server bound to `127.0.0.1`; it can be turned off as shown above.

## Known limitations

* Battery health data depends on what the device firmware itself provides; some models or system versions will report "unsupported". This is normal.
* The official after-sales tool's own implementation also varies across system versions; this module reads the same physical quantities with the same criteria, so the value should agree between the two channels.
* The grade ranges come from the official current configuration and are a snapshot of that version; if the official configuration changes later, this module will not follow automatically.
* Diagnostic items other than battery health (`030103`) — such as screen-related or Bluetooth earphone items — are out of scope.

## Disclaimer

This is an unofficial tool intended for personal learning and self-checking. The author accepts no responsibility for data deviations or device issues arising from its use. Please rely on the official after-sales diagnostic result.
