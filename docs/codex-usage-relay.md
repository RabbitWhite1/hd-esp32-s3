# Codex usage relay

The device needs a ChatGPT access token to fetch Codex usage. That token comes from a logged-in Codex
installation, so a computer running Codex periodically copies its current token to the ESP32.

The relay does not renew the token. It only reads the current value from `~/.codex/auth.json`; Codex
controls when that file is refreshed. The device decodes and displays the token expiry so an old token
is visible before it becomes confusing.

## Install the hourly relay

Install `curl` and `jq`, then run the following on the computer where Codex is logged in. Replace
`esp32.local` if the device uses another hostname.

```bash
(crontab -l 2>/dev/null; printf '%s\n' '0 * * * * codex_relay_token=$(jq -er .tokens.access_token "$HOME/.codex/auth.json") && curl -fsS --connect-timeout 5 --max-time 15 -X POST --data-urlencode "token=$codex_relay_token" http://esp32.local/codextoken >/dev/null || logger -t codex-relay "relay failed"') | crontab -
```

The command refuses a missing or null token, bounds network waits, suppresses successful output, and
writes failures to the system log without exposing the token.

To remove it:

```bash
crontab -l | grep -v codextoken | crontab -
```

## Relay once manually

Use this to test the complete path without printing the token:

```bash
codex_relay_token=$(jq -er '.tokens.access_token' "$HOME/.codex/auth.json") && \
  curl -fsS --connect-timeout 5 --max-time 15 -X POST \
  --data-urlencode "token=$codex_relay_token" http://esp32.local/codextoken
```

`Token saved` or `Token unchanged` means the device accepted it.

You can also paste `tokens.access_token` into **Configuration → Codex usage**, but it must be replaced
when it expires.

## Troubleshoot

Check the following in order:

```bash
crontab -l
systemctl is-active cron
journalctl -u cron --since today --grep codextoken
```

Then run the manual relay above and open the Codex card in the web UI. The card reports whether a token
is set and shows its decoded expiration time.

Cron logs prove that a job was launched, not that its HTTP request succeeded. The `logger` branch in
the recommended command records extraction, DNS, connection, timeout, and HTTP failures under the
`codex-relay` tag.

The usage service may return a different number or length of rate-limit windows for different plans.
The firmware labels gauges from the durations supplied by the service and allows the secondary window
to be absent.

## Security

The access token is sensitive. It is sent only over the local network to the device, but the device's
web endpoint is HTTP and the token is stored in plaintext in `/sdcard/esp32.json`. Use this integration
only on a trusted LAN, protect physical access to the card, and do not put the token in source control
or logs.
