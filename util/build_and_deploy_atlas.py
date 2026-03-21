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

python build_and_deploy_atlas.py [--use-asan] [--skip-test|--run-test] [--debug-version] [--release-pdb]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action="store_true", help="use sanitizers")
    tests = parser.add_mutually_exclusive_group()
    tests.add_argument(
        "--skip-test",
        dest="skip_test",
        action="store_true",
        help="skip building and running tests",
    )
    tests.add_argument(
        "--run-test",
        dest="skip_test",
        action="store_false",
        help="force building and running tests (overrides auto-skip on release tags)",
    )
    parser.set_defaults(skip_test=None)
    parser.add_argument("--debug-version", action="store_true", help="debug version")
    parser.add_argument(
        "--release-pdb",
        action="store_true",
        help="emit PDBs for optimized Release builds on Windows without disabling Release IPO/LTO",
    )
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
        release_pdb=args.release_pdb,
        enable_network_tests=args.enable_network_tests,
    )
    deploy_atlas.deploy_atlas(
        is_debug_version=args.use_asan or args.debug_version,
        release_pdb=args.release_pdb,
    )
