"""
Pre-build script to inject test Wi-Fi credentials from test_secrets.ini
This script is run by PlatformIO before building the test_provision environment.
"""
Import("env")
import os
import sys

def parse_simple_ini(filepath):
    """Parse a simple INI file without sections (KEY = value format)"""
    values = {}
    if not os.path.exists(filepath):
        return None
    
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            # Skip comments and empty lines
            if not line or line.startswith(';') or line.startswith('#'):
                continue
            
            # Parse KEY = value
            if '=' in line:
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip()
                values[key] = value
    
    return values

def escape_string_for_cpp(s):
    """Escape a string for C++ string literal"""
    # Escape backslashes first, then quotes
    s = s.replace('\\', '\\\\')
    s = s.replace('"', '\\"')
    return s

# Path to test secrets file
secrets_file = os.path.join(env.get("PROJECT_DIR"), "test", "test_secrets.ini")

print("=" * 60)
print("Injecting test secrets for hardware provisioning...")
print("=" * 60)

# Parse the secrets file
secrets = parse_simple_ini(secrets_file)

if secrets is None:
    print("\nERROR: Test secrets file not found!")
    print("Expected file: test/test_secrets.ini")
    print("\nPlease create it from the example:")
    print("  cp test/test_secrets.ini.example test/test_secrets.ini")
    print("  # Edit test/test_secrets.ini with your test credentials")
    print("\nNOTE: test_secrets.ini is gitignored and should never be committed.")
    sys.exit(1)

# Check for required keys
required_keys = ['WIFI_SSID', 'WIFI_PASS']
missing_keys = [key for key in required_keys if key not in secrets]

if missing_keys:
    print(f"\nERROR: Missing required keys in test/test_secrets.ini:")
    for key in missing_keys:
        print(f"  - {key}")
    print("\nPlease add all required keys to test/test_secrets.ini")
    print("See test/test_secrets.ini.example for format.")
    sys.exit(1)

# Escape and inject the secrets as build flags
# DO NOT print the actual values to avoid leaking credentials in build logs
ssid_escaped = escape_string_for_cpp(secrets['WIFI_SSID'])
pass_escaped = escape_string_for_cpp(secrets['WIFI_PASS'])

env.Append(CPPDEFINES=[
    ('TEST_WIFI_SSID', f'\\"{ssid_escaped}\\"'),
    ('TEST_WIFI_PASS', f'\\"{pass_escaped}\\"')
])

print(f"✓ WIFI_SSID loaded (length: {len(secrets['WIFI_SSID'])})")
print(f"✓ WIFI_PASS loaded (length: {len(secrets['WIFI_PASS'])})")
print("✓ Test secrets injected successfully")
print("=" * 60)
