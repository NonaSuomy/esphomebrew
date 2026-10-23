"""Guard ESP-Hosted 2.x AP-info responses during coprocessor updates."""
from pathlib import Path


def patch_hosted_rpc_source(path):
    path = Path(path)
    if not path.exists():
        return
    text = path.read_text()
    start = text.find('int rpc_wifi_sta_get_ap_info(')
    if start < 0:
        return
    end = text.index('\nint ', start + 4)
    function = text[start:end]
    old = 'if (resp && resp->resp_event_status == SUCCESS) {'
    new = '''/* PAX: an unsupported/malformed response has no AP record. */
\tif (resp && resp->resp_event_status == SUCCESS &&
\t    (resp->msg_id != RPC_ID__Resp_WifiStaGetApInfo ||
\t     !resp->u.wifi_scan_ap_list.out_list)) {
\t\trpc_rsp_callback(resp); /* release the response and any owned buffers */
\t\treturn ESP_FAIL;
\t}
\tif (resp && resp->resp_event_status == SUCCESS) {'''
    if '/* PAX: an unsupported/malformed response' in function:
        return
    if old not in function:
        raise RuntimeError('ESP-Hosted AP-info implementation changed; review compatibility guard')
    path.write_text(text[:start] + function.replace(old, new, 1) + text[end:])
    print('ESP-Hosted compatibility: reject AP-info responses without an AP record')
