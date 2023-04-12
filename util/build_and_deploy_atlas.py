import argparse

import build_atlas
import deploy_atlas


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python build_and_deploy_atlas.py [--use-asan] [--skip-test]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")
    parser.add_argument("--skip-test", action='store_true', help="skip test")
    args = parser.parse_args()

    build_atlas.build_atlas(use_asan=args.use_asan, skip_test=args.skip_test)
    deploy_atlas.deploy_atlas(use_asan=args.use_asan)
