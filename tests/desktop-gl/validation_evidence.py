"""Retain VVL messages independently of application debug callbacks and rendering."""
import hashlib
import re
from pathlib import Path


SETTINGS = '''khronos_validation.validate_sync = true
khronos_validation.report_flags = error,warn,info
khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG
khronos_validation.log_filename = validation.log
khronos_validation.enable_message_limit = false
'''


def validation_evidence(out, layer, manifest, device_hash):
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result = {'status': 'FAIL', 'scope': 'VVL messages; rendering is evaluated separately',
              'layer_sha256': sha(layer), 'manifest_sha256': sha(manifest),
              'settings_sha256': sha(out/'stage/vk_layer_settings.txt'),
              'collector_sha256': sha(Path(__file__))}
    try:
        log = (out/'validation.log').read_text(errors='replace')
        result['log_sha256'] = sha(out/'validation.log')
        result['device_log_sha256'] = device_hash
        result['vuids'] = sorted(set(re.findall(r'VUID-[A-Za-z0-9_-]+', log)))
        result['error_messages'] = log.count('Validation Error:')
        result['syncval_active'] = 'Current Enables: VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT' in log
        if device_hash != result['log_sha256']:
            raise ValueError('validation log device hash missing or mismatched')
        if 'layers/libVkLayer_khronos_validation.so' not in (out/'maps.txt').read_text():
            raise ValueError('validation layer not mapped')
        if not result['syncval_active']:
            raise ValueError('SyncVal activation not confirmed in independent log')
        if re.search(r'Validation Error|VUID-|SYNC-HAZARD', log):
            raise ValueError('Vulkan validation reported errors')
        result['status'] = 'PASS'
    except (OSError, ValueError) as error:
        result['error'] = str(error)
    return result
