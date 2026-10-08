# OPPO Battery Health (030103) — Magisk / KernelSU / APatch module

Reads battery health **exactly the way the official ColorOS diagnostic and the
O+ Support service tool do**, and nothing else. There are no compatibility
fallbacks: if an official data source is missing the result is
`unsupported`, which is what the official tool reports too.

## The official flow, and where each rule comes from

### Phone side — `com.oplus.postmanservice`

`ChargeDetectManager` (`g2/b.c`) hard-codes item 030103:

```java
if (Objects.equals(item, "030103"))
    result.add(com.oplus.postmanservice.charge.battery.detect.a.a(item));
else
    ...  // every other item goes through the XML stamp config
```

and `charge.battery.detect.a.a(String)` does:

```java
int    fcc    = OplusChargerHelper.getPsyBatteryFcc();
double design = new PowerProfile(ContextUtils.getContext()).getBatteryCapacity();
int    flag   = OplusChargerHelper.getIntegrateGaugeFccFlag();

double percent = (double) fcc / design * 100.0;   // constant 100.0

flag != 1 || percent <= 0  -> UNSUPPORTED   errorNo "t2"
percent <  60.0            -> ABNORMAL      errorNo "t03010302"
else                       -> NORMAL        errorNo "t03010301"

params[0] = String.valueOf(percent)
```

`OplusChargerHelper` resolves the two charger values as:

```java
int getPsyBatteryFcc()          { return invokeChargeInterface("getPsyBatteryFcc"); }
int getIntegrateGaugeFccFlag()  {
    int f = invokeChargeInterface("getIntegrateGaugeFccFlag");
    if (f != 1)                      // that AIDL method is absent in the
        return new File(             // shipped HAL, so this branch is taken
            "proc/charger/integrate_gauge_fcc_flag").exists() ? 1 : 0;
    return f;
}
```

`invokeChargeInterface` talks to
`vendor.oplus.hardware.charger.ICharger/default` (AIDL, transaction 29 for
`getPsyBatteryFcc`), whose implementation reads
`/sys/class/oplus_chg/battery/battery_fcc`.

### PC side — `ToolRecord.dll` / `DiagResultFilter.dll`

```c
// DiagResultObject::CalculatePercentFromCapacity(actual, design, flag)
if (actual > design && design > 0) actual = design;
if (flag != 1) return 0;
if (actual <= 0 || design <= 0) return 0;
return (int)(actual * 100.0 / design);

// BatteryChargingResultFilter::Execute
remainingBatteryCapacity -> default { good: 80, excellent: 60 }
                            special 20031,20061,19815,19525 -> good: 70
```

and the display logic in `DiagnosicEntry_mobile_*.json`:

```
percent >= good      -> 优
percent >= excellent -> 良
else                 -> 差        (item t03010302 prefixes it with 不通过)
```

`CalculatePercentFromCapacity` (with its `actual > design -> design` clamp) is
only reached through `ParseCapacityRatioFormat`, i.e. when the phone sends the
`flag;actual/design` form. The modern postmanservice plugin sends a plain
percentage, so on those devices the clamp never runs and this module does not
apply one either.

### Data sources used by this module (official only)

| Value | Path | Official equivalent |
|---|---|---|
| FCC | `/sys/class/oplus_chg/battery/battery_fcc` | `getPsyBatteryFcc()` |
| design | `power_profile.xml` → `battery.capacity` | `PowerProfile.getBatteryCapacity()` |
| flag | `/proc/charger/integrate_gauge_fcc_flag` | `getIntegrateGaugeFccFlag()` |

Two details that are easy to get wrong, and are matched exactly:

* **The flag only checks existence.** The official code is
  `new File(...).exists() ? 1 : 0` — it never reads the file's contents, so a
  file containing `0` still means `flag = 1`. (Verified: `getIntegrateGaugeFccFlag`
  appears **0 times** in every shipped charger interface — V1, V1.0 and V3 —
  so that branch is always taken.)
* **No rounding.** The phone computes `(double) fcc / design * 100.0` and the
  grade is compared against that unrounded value, while the value the PC
  records is the `(int)` cast of it. This module uses the unrounded value for
  the grade and reports the truncation separately as `percent_int`. So
  `59.999 %` is `不通过：差` (not rounded up to 60), and `79.996 %` is `良`
  (not rounded up to 优).

The power profile is looked up in the framework overlay order
(`/odm` → `/vendor` → `/system_ext` → `/system`); that is the same file the
framework's `PowerProfile` reads.

**No other path is ever used.** If one of the three is missing, or `flag != 1`,
or the ratio is `<= 0`, the module reports `unsupported`.

### A note on the DLLs

The five PC DLLs (`ToolRecord`, `DiagResultFilter`, `DiagnosticOld`,
`DiagnosticService`, `AdbCommand`) contain **no sysfs path at all** — grepping
them for `/sys/`, `power_supply`, `battery_fcc`, `design_capacity`,
`oplus_chg`, `power_profile` or `integrate_gauge_fcc_flag` returns nothing.
They define the arithmetic, the thresholds and the transport; the node paths
come from the phone side, which is where the table above was taken from.

## Output

The official detail text renders the grade plus a legend, and never prints the
percentage:

```js
if(result>=good) resultText+='优';
else if(result>=excellent) resultText+='良';
else resultText+='差';
resultText+='优：'+good+'-100%，良：'+excellent+'-'+good+'%，差：0-'+excellent+'%';
```

This module prints the grade and legend unchanged, and additionally shows the
raw data so the number can be read at a glance. The grade is always computed
from the **unrounded** ratio, exactly like the official code, so the two-decimal
display is presentational only:

```
$ su -c batteryhealth
电池健康 (030103)
  percent      = 76.65 %        (原始数据，两位小数)
  level        = 良
  legend       = 优：80-100%，良：60-80%，差：0-60%
  status       = normal
  item_no      = t03010301

  fcc          = 3550 mAh    [hal:hidl (app_process ChgFcc)]
  design       = 4510 mAh    [dumpsys batterystats (PowerProfile)]
  flag         = 1
  thresholds   = good>=80, excellent>=60   (project 19813)

  判级与记录（等级用未舍入值判定）:
    ratio_exact= 76.651884700699995
    record_int = 76
```

* `percent` — the ratio with 2 decimals (display only)
* `ratio_exact` — the unrounded `fcc / design * 100` the grade is computed from
* `record_int` — the `(int)` cast the PC stores in its record

Because the grade uses the unrounded value, a ratio such as `59.999 %` shows as
`60.00 %` but is still graded `不通过：差` — which is what the official tool does.

`status` mirrors the phone-side decision:

| status | item_no | meaning |
|---|---|---|
| `normal` | `t03010301` | phone returned NORMAL |
| `abnormal` | `t03010302` | percent < 60, phone returned ABNORMAL |
| `unsupported` | `t2` | flag != 1, or a node is missing |

`item_no` is the phone-side result/error code. It is not the diagnostic item
number; the item itself is `030103`.

Other modes:

```
su -c "batteryhealth --kv"            # key=value, used by the WebUI
su -c "batteryhealth --json"          # single line JSON
su -c "batteryhealth --kv --extra"    # adds telemetry that is NOT part of 030103
```

## Action button, WebUI and CLI

* **Action button** — Magisk / KernelSU / APatch draw one because the module
  ships `action.sh`. Tapping it starts the local page and opens it in the
  browser.
* **KernelSU / APatch** — open the module and tap *WebUI*.
* **Magisk** — no built-in WebUI host, so `service.sh` starts the module's own
  built-in HTTP server (no busybox dependency) on `127.0.0.1:8899` at boot;
  open `http://127.0.0.1:8899/` in any browser.
  Disable with `su -c "touch /data/adb/modules/oppo_battery_health/disable_http"`.
  A third-party host such as KsuWebUIStandalone or MMRL also works.

KernelSU may run the WebUI shell bridge on the WebView main thread. The page
yields one paint before invoking it, so the read button can visibly change to
读取中 before the command blocks the UI.

The page's **核心参数** block keeps the health fields without per-row hint
text. A separate **附加信息** section contains SOC, voltage, current,
temperature, charge state and cycle count. That telemetry never influences the
health value.

## Coverage

The flow above is device-independent: it probes the official nodes and uses
the project number only to pick the threshold list that
`DynamicConfig.json` already defines. There is no per-model branch, and no
substitute node is ever used.

Watches (item `030104`) and Bluetooth earbuds (`106119`) are different
diagnostic items and are out of scope.

## Known limits (stated honestly)

### 1. Design capacity — aligned

The official code calls `PowerProfile.getBatteryCapacity()`. `BatteryStatsService`
builds its `PowerProfile` the same way and prints the result, so the module now
reads that:

```
dumpsys batterystats -> "Estimated battery capacity: 4510 mAh"
```

That is the framework's resolved value (whatever APK/overlay it came from), not
a guess at a file path. The overlay XML is only a fallback for when the dump is
unavailable, and both produce `design_src` so the origin is always visible.

### 2. FCC via the charger HAL — official sequence reproduced in full

The official `OplusChargerHelper` does:

```java
getOplusCharger():
    1) AIDL  vendor.oplus.hardware.charger.ICharger/default
             ICharger$Stub.asInterface(ServiceManager.getService(...))
    2) HIDL  vendor.oplus.hardware.charger.V1_0.ICharger.getService()
invokeChargeInterface("getPsyBatteryFcc"):
    svc.getPsyBatteryFcc()      // reflection
    -> -1 on any failure
```

A shell cannot reach hwbinder, so the module ships a tiny DEX
(`bin/chgfcc.dex`, source in `src/ChgFcc.java`) and runs it through
`app_process`. That DEX performs exactly the two lookups above — AIDL first,
then HIDL — and calls `getPsyBatteryFcc()` reflectively, so the real sequence
runs unchanged:

```
CLASSPATH=<dex>:<vendor.oplus.hardware.charger-*-java.jar> \
    app_process /system/bin ChgFcc
  -> fcc=3550
     via=hidl
```

`ChgFcc.java` references no `android.*` types at compile time (pure reflection),
so it builds with a plain JDK:

```
javac --release 8 -d out src/ChgFcc.java
d8 --release --lib android.jar --min-api 26 --output out out/ChgFcc.class
```

Only if that fails (missing dex, no `app_process`, missing HAL jars) does the
module fall back to reading `/sys/class/oplus_chg/battery/battery_fcc` — the
only FCC path in the HAL binary, i.e. exactly what `getPsyBatteryFcc()` reads
internally.

`fcc_src` always states which of the two was used (`hal:hidl`, `hal:aidl` or
`/sys/... (HAL unavailable)`), and `fcc_hal`, `fcc_hal_via` and `fcc_node`
expose both readings side by side so nothing is hidden.

Both ROM slots were verified on the same device:

| Slot | Charger HAL | Module result |
|---|---|---|
| A | AIDL V3 (`charger-V3-service`) | `service call` trans 29 → 3436; node 3436 |
| B | HIDL V1.0 (`charger@1.0-service`) | `app_process ChgFcc` → `via=hidl`, fcc 3550; node 3550 |

Both slots give the HAL value, and it equals the node the HAL reads.

### 3. Threshold list is a snapshot

`20031 / 20061 / 19815 / 19525 → good 70` is copied from `DynamicConfig.json`.
Only one copy of that file exists on disk (the bundled one, 2026-10-02), so this
is the current config; a future server-side refresh would not be picked up.

### 4. Phone-side implementations differ across ROM versions

The 030103 handler is plugin-version dependent:

| PostmanService | 030103 handling |
|---|---|
| 14.4.0 | hard-coded: `charge.battery.detect.a.a("030103")` → `getPsyBatteryFcc()` + `PowerProfile` |
| 13.3.5 | not in `detect_item_config.xml` at all — the tool falls back to its older channel |

Both channels end up with the same physical quantities (full-charge capacity and
design capacity), which is what this module reads directly, so the value is the
same either way.

### 5. Telemetry is an addition

Voltage / current / temperature / cycle count are printed only with `--extra`
(and in the WebUI's separately labelled *附加信息（非 030103）* block). They never
influence the health value. The kernel reports `POWER_SUPPLY_VOLTAGE_NOW` in
microvolts and `POWER_SUPPLY_CURRENT_NOW` in microamperes; the module converts
each to millivolts and milliamperes for display.

The WebUI, the local web server and the action button are delivery
mechanisms; they do not read or transform the battery value.

## Operational notes

* **The action button only opens the page.** `batteryhealth --action` starts the
  built-in server and fires `am start`; it reads nothing and exits immediately.
  The page loads idle and reads only when you press 读取电池健康. Measured on
  the OnePlus 8 Pro:

  | step | before | now |
  |---|---|---|
  | `action.sh` total | 1816 ms | **175 ms** |
  | browser on top (from tap) | ~3–4 s | **~265 ms** |
  | reading | in the tap path | 1792 ms, only when the page asks |

  The liveness probe fetches a static file (`webroot/ping.txt`) instead of the
  CGI, because probing the CGI would run a whole reading.
* **No notification and no result files.** Earlier versions posted a
  notification through `cmd notification post` and wrote `battery_health.txt`.
  Both are gone: the notification because most ROMs silently drop shell-posted
  notifications (on ColorOS the command reports success while
  `dumpsys notification` never shows a record for it), the files because the
  script's own output is all that is needed. `batteryhealth` now only prints —
  it writes nothing anywhere.
* **Repeated calls are fine — an earlier note here was a misdiagnosis.** It
  claimed ColorOS' anti-root locks the module directory per shell session.
  Re-tested: five consecutive `sh .../batteryhealth` calls in one shell all
  succeeded, and the module carries the same label as any other module
  (`u:object_r:system_file:s0`). The `EACCES` observed at the time came from
  writing into `/data/adb/modules/<id>` by hand while the device was still
  booting; a normal flash does not hit it. `action.sh` uses one `--action` call
  only because that puts the steps in the right order (serve → open → read).
* **The action button needs the screen on.** `am start` cannot bring the
  browser forward while the device is asleep; with the screen on it works
  (verified: `topResumedActivity=com.heytap.browser/...BrowserActivity`).
  Tapping the button in the Magisk app always satisfies this.
* The `--serve-stop` path deliberately avoids `pkill -f`; a broad pattern
  match was observed to disturb the calling shell on this ROM.
