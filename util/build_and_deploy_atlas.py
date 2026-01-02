import argparse

import build_atlas
import deploy_atlas
from logger import setup_logger

if __name__ == "__main__":
    logger = setup_logger()
    deploy_atlas.load_deploy_env_from_dotenv()

    parser = argparse.ArgumentParser(
        epilog="""
Examples:

python build_and_deploy_atlas.py [--use-asan] [--skip-test] [--debug-version]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")
    parser.add_argument("--skip-test", action='store_true', help="skip test")
    parser.add_argument("--debug-version", action='store_true', help="debug version")
    parser.add_argument(
        "--enable-network-tests",
        action="store_true",
        help="enable network-dependent tests (disabled by default for CI/firewalled environments)",
    )
    args = parser.parse_args()

    build_atlas.build_atlas(
        use_asan=args.use_asan,
        skip_test=args.skip_test,
        debug_version=args.debug_version,
        enable_network_tests=args.enable_network_tests,
    )
    deploy_atlas.deploy_atlas(is_debug_version=args.use_asan or args.debug_version)
