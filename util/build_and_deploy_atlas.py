import argparse

import build_atlas
import deploy_atlas


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python build_and_deploy_atlas.py [--use-asan] [--skip-tests]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")
    parser.add_argument("--skip-tests", action='store_true', help="skip tests")
    args = parser.parse_args()

    build_atlas.build_atlas(use_asan=args.use_asan, skip_tests=args.skip_tests)
    deploy_atlas.deploy_atlas(use_asan=args.use_asan)
