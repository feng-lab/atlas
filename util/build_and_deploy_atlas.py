import argparse

import build_atlas
import deploy_atlas
import common_dirs

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python build_and_deploy_atlas.py [--use-asan] [--skip-test] [--debug-version]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")
    parser.add_argument("--skip-test", action='store_true', help="skip test")
    parser.add_argument("--debug-version", action='store_true', help="debug version")
    args = parser.parse_args()

    if common_dirs.is_mac():
        build_atlas.build_atlas(use_asan=args.use_asan, skip_test=args.skip_test, debug_version=args.debug_version,
                                arm64=True)
    build_atlas.build_atlas(use_asan=args.use_asan, skip_test=args.skip_test, debug_version=args.debug_version,
                            arm64=False)
    deploy_atlas.deploy_atlas(is_debug_version=args.use_asan or args.debug_version)
