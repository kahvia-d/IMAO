const forward = detail => {
  if (!detail || typeof detail.token !== "string") return;
  chrome.runtime.sendMessage({
    type: "sessionCandidate",
    accountId: String(detail.accountId || ""),
    token: detail.token,
    diagnostic: String(detail.diagnostic || "")
  }).catch(() => { });
};

window.addEventListener("imao-kuro-session", event => forward(event.detail));

// Ask immediately and a few more times: this listener may be attached after the
// page script published the session, and a single-page app may only write the
// token after its first render.
const request = () => window.dispatchEvent(new CustomEvent("imao-kuro-session-request"));
request();
let attempts = 0;
const timer = setInterval(() => {
  request();
  if (++attempts >= 6) clearInterval(timer);
}, 1000);
