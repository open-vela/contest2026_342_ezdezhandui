#!/bin/bash
# pre-commit hook: Check if API docs are updated when header files change
# Install: cp scripts/hooks/check-api-doc-sync.sh .git/hooks/pre-commit

# Header file -> API doc mapping
declare -A HEADER_DOC_MAP=(
    # ========== Kernel ==========
    ["nuttx/include/pthread.h"]="docs/zh-cn/api/kernel/thread.md"
    ["nuttx/include/sched.h"]="docs/zh-cn/api/kernel/sched.md"
    ["nuttx/include/signal.h"]="docs/zh-cn/api/kernel/signal.md"
    ["nuttx/include/mqueue.h"]="docs/zh-cn/api/kernel/msgqueue.md"
    ["nuttx/include/nuttx/mm/mm.h"]="docs/zh-cn/api/kernel/mem.md"

    # ========== Network ==========
    ["nuttx/include/sys/socket.h"]="docs/zh-cn/api/network/net.md"
    ["nuttx/include/netdb.h"]="docs/zh-cn/api/network/net.md"
    ["nuttx/include/nuttx/net/dns.h"]="docs/zh-cn/api/network/net.md"

    # ========== Bluetooth ==========
    ["frameworks/connectivity/bluetooth/framework/include/bt_adapter.h"]="docs/zh-cn/api/framework/bluetooth/bt_gap.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_device.h"]="docs/zh-cn/api/framework/bluetooth/bt_device.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_a2dp.h"]="docs/zh-cn/api/framework/bluetooth/bt_a2dp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_a2dp_sink.h"]="docs/zh-cn/api/framework/bluetooth/bt_a2dp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_a2dp_source.h"]="docs/zh-cn/api/framework/bluetooth/bt_a2dp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_gattc.h"]="docs/zh-cn/api/framework/bluetooth/bt_gatt.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_gatts.h"]="docs/zh-cn/api/framework/bluetooth/bt_gatt.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_hfp_hf.h"]="docs/zh-cn/api/framework/bluetooth/bt_hfp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_hfp_ag.h"]="docs/zh-cn/api/framework/bluetooth/bt_hfp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_hfp.h"]="docs/zh-cn/api/framework/bluetooth/bt_hfp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_hid_device.h"]="docs/zh-cn/api/framework/bluetooth/bt_hid.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_pan.h"]="docs/zh-cn/api/framework/bluetooth/bt_pan.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_spp.h"]="docs/zh-cn/api/framework/bluetooth/bt_spp.md"
    ["frameworks/connectivity/bluetooth/framework/include/bt_cs.h"]="docs/zh-cn/api/framework/bluetooth/bt_cs.md"

    # ========== Multimedia ==========
    ["frameworks/multimedia/media/include/media_player.h"]="docs/zh-cn/api/framework/media/media_player.md"
    ["frameworks/multimedia/media/include/media_recorder.h"]="docs/zh-cn/api/framework/media/media_recorder.md"
    ["frameworks/multimedia/media/include/media_focus.h"]="docs/zh-cn/api/framework/media/media_focus.md"
    ["frameworks/multimedia/media/include/media_policy.h"]="docs/zh-cn/api/framework/media/media_policy.md"
    ["frameworks/multimedia/media/include/media_session.h"]="docs/zh-cn/api/framework/media/media_session.md"
    ["frameworks/multimedia/media/include/media_trigger.h"]="docs/zh-cn/api/framework/media/media_trigger.md"
    ["frameworks/multimedia/media/include/media_trigger_model.h"]="docs/zh-cn/api/framework/media/media_trigger_model.md"
    ["frameworks/multimedia/media/include/media_utils.h"]="docs/zh-cn/api/framework/media/media_utils.md"

    # ========== Telephony ==========
    ["frameworks/connectivity/telephony/include/tapi_manager.h"]="docs/zh-cn/api/framework/telephony/telephony_manager.md"
    ["frameworks/connectivity/telephony/include/tapi_call.h"]="docs/zh-cn/api/framework/telephony/telephony_call.md"
    ["frameworks/connectivity/telephony/include/tapi_sms.h"]="docs/zh-cn/api/framework/telephony/telephony_sms.md"
    ["frameworks/connectivity/telephony/include/tapi_data.h"]="docs/zh-cn/api/framework/telephony/telephony_data.md"
    ["frameworks/connectivity/telephony/include/tapi_network.h"]="docs/zh-cn/api/framework/telephony/telephony_network.md"
    ["frameworks/connectivity/telephony/include/tapi_sim.h"]="docs/zh-cn/api/framework/telephony/telephony_sim.md"
    ["frameworks/connectivity/telephony/include/tapi_ims.h"]="docs/zh-cn/api/framework/telephony/telephony_ims.md"
    ["frameworks/connectivity/telephony/include/tapi_ss.h"]="docs/zh-cn/api/framework/telephony/telephony_ss.md"
    ["frameworks/connectivity/telephony/include/tapi_stk.h"]="docs/zh-cn/api/framework/telephony/telephony_stk.md"
    ["frameworks/connectivity/telephony/include/tapi_phonebook.h"]="docs/zh-cn/api/framework/telephony/telephony_phonebook.md"
    ["frameworks/connectivity/telephony/include/tapi_phone.h"]="docs/zh-cn/api/framework/telephony/telephony_phone.md"
    ["frameworks/connectivity/telephony/include/tapi_cbs.h"]="docs/zh-cn/api/framework/telephony/telephony_cbs.md"

    # ========== System Framework ==========
    ["apps/system/uorb/uORB/uORB.h"]="docs/zh-cn/api/framework/uorb.md"
    ["frameworks/system/utils/include/kvdb.h"]="docs/zh-cn/api/framework/kvdb.md"
)

warnings=0
changed_files=$(git diff --cached --name-only)

for header in "${!HEADER_DOC_MAP[@]}"; do
    doc="${HEADER_DOC_MAP[$header]}"
    # Check if the header file is changed in this commit
    if echo "$changed_files" | grep -q "$header"; then
        # Check if the corresponding doc is also changed
        if ! echo "$changed_files" | grep -q "$doc"; then
            echo "WARNING: $header changed but API doc $doc not updated"
            warnings=$((warnings + 1))
        fi
    fi
done

if [ "$warnings" -gt 0 ]; then
    echo ""
    echo "Detected $warnings header file change(s) without API doc sync."
    echo "Please update the corresponding API docs before committing."
    echo "Use 'git commit --no-verify' to skip this check if no doc update is needed."
    exit 1
fi

exit 0
