# elotto_ota
Update endpoint + boot safety (`BOOT_ATTEMPT_LIMIT` 3).
⚠ `fw_boot_attempts` counts boots since the last run that stayed up 30 s, **not** failures — every
node reads 1 for its first 30 s after a flash.
