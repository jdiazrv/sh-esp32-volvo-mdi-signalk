"""Apply the project's audited SensESP persistence fixes before compilation.

PlatformIO installs registry dependencies in its configured libdeps directory.
Keeping this idempotent patcher in the project makes clean builds receive the
same fixes instead of relying on a manually modified package cache.
"""

from pathlib import Path

Import("env")  # type: ignore[name-defined]  # supplied by PlatformIO/SCons


def replace_once(path: Path, old, new: str, skip_markers=()) -> None:
    """Replace the first match of `old` with `new`, idempotently.

    `old` may be a single snippet or a list of snippets: the pristine SensESP
    source plus any body this script produced in an earlier revision. That way
    a libdeps tree already carrying an older version of the patch is upgraded
    in place instead of aborting the build.
    """
    candidates = [old] if isinstance(old, str) else list(old)
    text = path.read_text()
    # `skip_markers` identify states where this particular replacement must not
    # run: either the file already reached its final form (possibly via another
    # call, whose body differs only in comments) or this replacement simply does
    # not apply to the revision on disk. Either way it is not an error.
    if new in text or any(marker in text for marker in skip_markers):
        return
    for candidate in candidates:
        if candidate in text:
            path.write_text(text.replace(candidate, new, 1))
            print(f"Patched SensESP persistence: {path.name}")
            return
    raise RuntimeError(f"SensESP source changed; cannot safely patch {path}")


libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
environment = env.subst("$PIOENV")
sensesp = libdeps / environment / "SensESP" / "src" / "sensesp"

saveable = sensesp / "system" / "saveable.cpp"
config_handler = sensesp / "net" / "web" / "config_handler.cpp"

# A libdeps tree already carrying the previous revision of this patch no longer
# contains the pristine SensESP body, so the call above cannot match it. Upgrade
# just the verification block in that case. Checking size() on the handle that
# created the file is unreliable: VFSFileImpl only initialises its cached stat
# when the file already existed at open time, so a freshly created file could
# report a stale size and fail a save that had actually succeeded.
replace_once(
    saveable,
    """  size_t written = f.print(serialized_config);
  f.flush();
  size_t stored_size = f.size();
  f.close();
  if (written != serialized_config.length() ||
      stored_size != serialized_config.length()) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Incomplete configuration write for %s",
             config_path_.c_str());
    return false;
  }

  File verify_file = SPIFFS.open(temporary_path, "r");
  JsonDocument verify_doc;
  DeserializationError verify_error = deserializeJson(verify_doc, verify_file);
  verify_file.close();
  if (verify_error) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Configuration verification failed for %s",
             config_path_.c_str());
    return false;
  }""",
    """  size_t written = f.print(serialized_config);
  f.flush();
  f.close();
  if (written != serialized_config.length()) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Short configuration write for %s: %u of %u bytes",
             config_path_.c_str(), (unsigned)written,
             (unsigned)serialized_config.length());
    return false;
  }

  File verify_file = SPIFFS.open(temporary_path, "r");
  if (!verify_file) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Could not reopen configuration for %s",
             config_path_.c_str());
    return false;
  }
  const size_t stored_size = verify_file.size();
  JsonDocument verify_doc;
  DeserializationError verify_error = deserializeJson(verify_doc, verify_file);
  verify_file.close();
  if (verify_error || stored_size != serialized_config.length()) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__,
             "Configuration verification failed for %s: %u of %u bytes, %s",
             config_path_.c_str(), (unsigned)stored_size,
             (unsigned)serialized_config.length(),
             verify_error ? verify_error.c_str() : "size mismatch");
    return false;
  }""",
    skip_markers=(
        # Already in final form, written by the pristine replacement below.
        "Short configuration write for %s",
        # An untouched SensESP tree: this upgrade does not apply, the pristine
        # replacement below handles it.
        "// Delete any existing configuration files",
    ),
)

replace_once(
    saveable,
    '''  String hash_path = String("/") + Base64Sha1(config_path_);

  // Delete any existing configuration files
  String filename;
  if (find_config_file(config_path_, filename)) {
    SPIFFS.remove(filename);
  }

  JsonDocument json_doc;
  JsonObject obj = json_doc.to<JsonObject>();
  if (!to_json(obj)) {
    ESP_LOGW(__FILENAME__, "Could not get configuration from json for %s",
             config_path_.c_str());
    return false;
  }
  File f = SPIFFS.open(hash_path, "w");
  serializeJson(obj, f);
  f.close();

  String str;
  serializeJson(obj, str);
  ESP_LOGV(__FILENAME__, "Configuration saved for %s: %s", config_path_.c_str(),
           str.c_str());

  return true;''',
    '''  String hash_path = String("/") + Base64Sha1(config_path_);
  String temporary_path = hash_path + ".n";
  String backup_path = hash_path + ".o";

  JsonDocument json_doc;
  JsonObject obj = json_doc.to<JsonObject>();
  if (!to_json(obj) || json_doc.overflowed()) {
    ESP_LOGW(__FILENAME__, "Could not serialize configuration for %s",
             config_path_.c_str());
    return false;
  }
  String serialized_config;
  serializeJson(obj, serialized_config);

  // Write and verify a new file before touching the working configuration.
  SPIFFS.remove(temporary_path);
  File f = SPIFFS.open(temporary_path, "w");
  if (!f) {
    ESP_LOGE(__FILENAME__, "Could not open temporary configuration for %s",
             config_path_.c_str());
    return false;
  }
  size_t written = f.print(serialized_config);
  f.flush();
  f.close();
  if (written != serialized_config.length()) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Short configuration write for %s: %u of %u bytes",
             config_path_.c_str(), (unsigned)written,
             (unsigned)serialized_config.length());
    return false;
  }

  // Re-open and parse the file back. Checking size() on the handle that
  // created it is not reliable: VFSFileImpl only initialises its cached stat
  // when the file already existed at open time, so a freshly created file can
  // report a stale size. Once the file exists the constructor stats it for
  // real, and parsing it back is a stronger guarantee than any length check.
  File verify_file = SPIFFS.open(temporary_path, "r");
  if (!verify_file) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Could not reopen configuration for %s",
             config_path_.c_str());
    return false;
  }
  const size_t stored_size = verify_file.size();
  JsonDocument verify_doc;
  DeserializationError verify_error = deserializeJson(verify_doc, verify_file);
  verify_file.close();
  if (verify_error || stored_size != serialized_config.length()) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__,
             "Configuration verification failed for %s: %u of %u bytes, %s",
             config_path_.c_str(), (unsigned)stored_size,
             (unsigned)serialized_config.length(),
             verify_error ? verify_error.c_str() : "size mismatch");
    return false;
  }

  // Swap only after the replacement is complete. Restore the old file if the
  // final rename fails, so a failed Save can never erase working credentials.
  String old_filename;
  bool had_old = find_config_file(config_path_, old_filename);
  SPIFFS.remove(backup_path);
  if (had_old && !SPIFFS.rename(old_filename, backup_path)) {
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Could not protect old configuration for %s",
             config_path_.c_str());
    return false;
  }
  if (!SPIFFS.rename(temporary_path, hash_path)) {
    if (had_old) SPIFFS.rename(backup_path, old_filename);
    SPIFFS.remove(temporary_path);
    ESP_LOGE(__FILENAME__, "Could not activate configuration for %s",
             config_path_.c_str());
    return false;
  }
  if (had_old) SPIFFS.remove(backup_path);

  ESP_LOGV(__FILENAME__, "Configuration saved for %s: %s", config_path_.c_str(),
           serialized_config.c_str());
  return true;''',
    skip_markers=("Short configuration write for %s",),
)



replace_once(
    config_handler,
    '''        std::unique_ptr<char[]> payload(new char[payload_len + 1]);
        int ret = httpd_req_recv(req, payload.get(), payload_len);
        if (ret <= 0) {
          httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                              "Error receiving payload");
          return ESP_FAIL;
        }
        payload[payload_len] = '\\0';''',
    '''        std::unique_ptr<char[]> payload(new char[payload_len + 1]);
        size_t received = 0;
        uint8_t receive_timeouts = 0;
        while (received < payload_len) {
          int ret = httpd_req_recv(req, payload.get() + received,
                                   payload_len - received);
          if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++receive_timeouts < 5) continue;
          if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Error receiving payload");
            return ESP_FAIL;
          }
          received += static_cast<size_t>(ret);
          receive_timeouts = 0;
        }
        payload[received] = '\\0';''',
)

replace_once(
    config_handler,
    '''        config_item->save();
        response = "{\\"status\\":\\"ok\\"}";''',
    '''        if (!config_item->save()) {
          ESP_LOGE("ConfigHandler", "Error persisting configuration");
          httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                              "Configuration could not be saved");
          return ESP_FAIL;
        }
        response = "{\\"status\\":\\"ok\\"}";''',
)
