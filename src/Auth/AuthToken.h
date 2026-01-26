const char *face_json_url = "REPLACE_WITH_YOUR_URL";
const char *face_checksum_url = "REPLACE_WITH_YOUR_URL";
// Optional: fallback to Gitee for face data (private repo supported via token).
const char *face_gitee_json_url = "REPLACE_WITH_YOUR_URL";
const char *face_gitee_checksum_url = "REPLACE_WITH_YOUR_URL";
const char *face_repo_token = "REPLACE_WITH_YOUR_TOKEN"; // Set to your Gitee personal access token when needed.
const char *gitee_accept_header = "application/vnd.github.v3.raw";

// Base URL for device-scoped user_config.json; the device_id and ".json" are appended.
const char *user_config_base_url = "REPLACE_WITH_YOUR_URL";
// Replace with a GitHub token that has read access to the private repo; leave empty for public repos.
const char *user_config_github_token = "REPLACE_WITH_YOUR_TOKEN";
// Optional Gitee source for user_config.json (private repo supported via token).
const char *user_config_gitee_base_url = "REPLACE_WITH_YOUR_URL";
const char *user_config_gitee_token = "REPLACE_WITH_YOUR_TOKEN"; // Set to your Gitee personal access token when needed.