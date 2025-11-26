#!/usr/bin/env python3

from ast import arg
import os
import sys
import argparse
import subprocess
from xmlrpc import client
from google.cloud import compute_v1
from google.oauth2 import service_account

# Platform to (image mapping, git install command)
image_map = {
    'rhel8': ('projects/rhel-cloud/global/images/family/rhel-8', 'sudo dnf update -y && sudo dnf install -y git'),
    'rhel9': ('projects/rhel-cloud/global/images/family/rhel-9', 'sudo dnf update -y && sudo dnf install -y git'),
    'rhel10': ('projects/rhel-cloud/global/images/family/rhel-10', 'sudo dnf update -y && sudo dnf install -y git'),
    'ubuntu18.04': ('projects/ubuntu-os-cloud/global/images/family/ubuntu-1804-lts', 'sudo apt update -y && sudo apt-get install -y git'),
    'ubuntu20.04': ('projects/ubuntu-os-cloud/global/images/family/ubuntu-2004-lts', 'sudo apt update -y && sudo apt-get install -y git'),
}

def parse_cmd():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description='Remote build and test script for Kudu on GCP'
    )
    
    parser.add_argument(
        '-p', '--platform',
        type=str,
        required=True,
        choices=list(image_map.keys()),
        help='Platform to build on (rhel8/9/10, ubuntu18.04/20.04)'
    )
    
    parser.add_argument(
        '-k', '--key',
        type=str,
        default=None,
        help='GCP service account key file'
    )
    
    parser.add_argument(
        '--build_type',
        type=str,
        required=True,
        choices=['debug', 'tsan', 'asan', 'release'],
        help='Build type (debug/tsan/asan/release)'
    )

    parser.add_argument(
        '--instance-name',
        type=str,
        default='temp-kudu-agent',
        help='GCP instance name (default: temp-kudu-agent)'
    )
    
    parser.add_argument(
        '--zone',
        type=str,
        default='us-central1-a',
        help='GCP zone (default: us-central1-a)'
    )
    
    parser.add_argument(
        '--architecture',
        type=str,
        default='x64',
        choices=['arm', 'x64'],
        help='Architecture (arm/x64, default: x64)'
    )
    
    parser.add_argument(
        '--repo',
        type=str,
        default='https://github.com/apache/kudu',
        help='Repository URL (default: https://github.com/apache/kudu)'
    )
    
    parser.add_argument(
        '--commit',
        type=str,
        default=None,
        help='Specific commit to checkout (default: None, uses default branch)'
    )
    
    parser.add_argument(
        '--allow-slow',
        action='store_true',
        default=True,
        help='Allow slow tests to run (default: True)'
    )
    
    args = parser.parse_args()
    
    if not args.key:
        args.key = os.environ.get("GCP_KEY") or None
        
    return args


def initialize_gcp_client(args):
    """Initialize GCP Compute client with service account key."""
    gcp_key = args.key
    if not gcp_key:
        raise Exception("Error: neither GCP_KEY environment variable nor --key argument set")
    if not os.path.exists(gcp_key):
        raise Exception(f"Error: Service account key file not found: {gcp_key}")
    credentials = service_account.Credentials.from_service_account_file(gcp_key)
    client = compute_v1.InstancesClient(credentials=credentials)
    return client, credentials.project_id


def check_instance_exists(args):
    try:
        request = compute_v1.GetInstanceRequest(
            project=args.project_id,
            zone=args.zone,
            instance=args.instance_name
        )

        args.client.get(request=request)
        return True
    
    except Exception as e:
        if "was not found" in str(e).lower():
            return False
        else:
            raise Exception(f"Error checking if instance {args.instance_name} exists: {e}")


def check_instance_status(args): 
    try:
        # Get instance details
        request = compute_v1.GetInstanceRequest(
            project=args.project_id,
            zone=args.zone,
            instance=args.instance_name
        )

        instance = args.client.get(request=request)
        status = instance.status
        print(f"Instance {args.instance_name} status: {status}")
        
        return status == "RUNNING"
    
    except Exception as e:
        raise Exception(f"Error checking instance {args.instance_name}: {e}")


def delete_instance(args):
    try:
        if not args.instance_name.startswith('temp-'):
            raise Exception(f"Refusing to delete non-temporary instance {args.instance_name}")
        request = compute_v1.DeleteInstanceRequest(
            project=args.project_id,
            zone=args.zone,
            instance=args.instance_name
        )

        operation = args.client.delete(request=request)
        print(f"Waiting for instance deletion to complete...")
        operation.result()  # Wait for the operation to complete
        print(f"Instance {args.instance_name} has been deleted")
        return True
    
    except Exception as e:
        raise Exception(f"Error deleting instance {args.instance_name}: {e}")


def create_instance(args):
    """Create a new GCP instance."""
    print(f"Creating instance: {args.instance_name}")
    
    # Determine machine type based on architecture
    if args.architecture == 'arm':
        machine_type = 't2a-standard-4'  # ARM-based machine
    else:
        machine_type = 'n2-standard-4'  # x64-based machine
    
    # Platform to image mapping
    
    
    if not args.platform in image_map:
        raise Exception(f"Unknown platform: {args.platform}")
    source_image = image_map.get(args.platform)[0]

    # Configure the instance
    instance = compute_v1.Instance()
    instance.name = args.instance_name
    instance.machine_type = f"zones/{args.zone}/machineTypes/{machine_type}"
    
    # Boot disk configuration
    disk = compute_v1.AttachedDisk()
    disk.boot = True
    disk.auto_delete = True
    disk.initialize_params = compute_v1.AttachedDiskInitializeParams()
    disk.initialize_params.source_image = source_image
    disk.initialize_params.disk_size_gb = 100
    disk.initialize_params.disk_type = f"zones/{args.zone}/diskTypes/pd-standard"
    instance.disks = [disk]
    
    # Network configuration
    network_interface = compute_v1.NetworkInterface()
    network_interface.name = 'default'
    access_config = compute_v1.AccessConfig()
    access_config.name = 'External NAT'
    access_config.type_ = 'ONE_TO_ONE_NAT'
    network_interface.access_configs = [access_config]
    instance.network_interfaces = [network_interface]
    
    try:
        request = compute_v1.InsertInstanceRequest()
        request.project = args.project_id
        request.zone = args.zone
        request.instance_resource = instance

        operation = args.client.insert(request=request)
        print(f"Waiting for instance creation to complete...")
        operation.result()  # Wait for the operation to complete
        print(f"Instance {args.instance_name} has been created")
        return True
    
    except Exception as e:
        raise Exception(f"Error creating instance {args.instance_name}: {e}")

# We don't have what we need in the python wrapper
def authenticate_gcloud_cmd(args):
    subprocess.check_call([
        'gcloud', 'auth', 'activate-service-account',
        '--key-file', args.key])
    subprocess.check_call(['gcloud', 'config', 'set', 'project', args.project_id])
    

def execute_remote_command(args, command):
    print(f"Executing command on {args.instance_name}: {command}")
    
    # Build the gcloud ssh command
    ssh_command = [
        'gcloud', 'compute', 'ssh',
        args.instance_name,
        '--project', args.project_id,
        '--zone', args.zone,
        '--command', command
    ]
    
    result = subprocess.check_call(
        ssh_command,
        stdout=sys.stdout,
        stderr=sys.stderr,
        text=True
    )
        

def copy_file_to_instance(args, local_path, remote_path):
    print(f"Copying {local_path} to {args.instance_name}:{remote_path}")
    
    scp_command = [
        'gcloud', 'compute', 'scp',
        local_path,
        f"{args.instance_name}:{remote_path}",
        '--project', args.project_id,
        '--zone', args.zone
    ]
    
    result = subprocess.check_call(
        scp_command,
        stdout=sys.stdout,
        stderr=sys.stderr,
        text=True
    )
        

def copy_file_from_instance(args, remote_path, local_path):
    print(f"Copying {args.instance_name}:{remote_path} to {local_path}")
    
    scp_command = [
        'gcloud', 'compute', 'scp',
        f"{args.instance_name}:{remote_path}",
        local_path,
        '--project', args.project_id,
        '--zone', args.zone
    ]
    
    result = subprocess.check_call(
        scp_command,
        stdout=sys.stdout,
        stderr=sys.stderr,
        text=True
    )


def main():
    args = parse_cmd()
    args.client, args.project_id = initialize_gcp_client(args)
    does_exist = check_instance_exists(args)
    print(f"Old instance exists: {does_exist}")
    if does_exist:
        # Previus job aborted
        delete_instance(args)

    try:
        create_instance(args)
        execute_remote_command(args, "echo 'Hello from remote instance!'")
        delete_instance(args)
    finally:
        pass

def build_test_script(args):
    script = []
    git_install_cmd = image_map.get(args.platform)[1]
    script.append("#!/bin/bash")
    script.append("")
    script.append("set -e")
    script.append(git_install_cmd)
    script.append("rm -rf ~/kudu_workspace")
    script.append("mkdir ~/kudu_workspace")
    script.append(f"git clone {args.repo} ~/kudu_workspace/kudu")
    script.append("cd ~/kudu_workspace/kudu")
    if args.commit:
        script.append(f"git checkout {args.commit}")
    script.append("sudo ./docker/bootstrap-dev-env.sh && sudo ./docker/bootstrap-java-env.sh")
    script.append(f"export KUDU_ALLOW_SLOW_TESTS={1 if args.allow_slow else 0}")
    script.append(f"./build-support/jenkins/build-and-test.sh")
    return "\n".join(script)

def main_cut():
    args = parse_cmd()
    args.client, args.project_id = initialize_gcp_client(args)
    does_exist = check_instance_exists(args)
    print(f"Old instance exists: {does_exist}")
    if not does_exist:
        create_instance(args)

    authenticate_gcloud_cmd(args)
    build_and_test_script = build_test_script(args)    
    with open('kudu_script.sh', 'w') as f:
        f.write(build_and_test_script)

    copy_file_to_instance(args, 'kudu_script.sh', '~/kudu_script.sh')
    execute_remote_command(args, "chmod +x ~/kudu_script.sh && ~/kudu_script.sh")
    copy_file_from_instance(args, '~/kudu_workspace/kudu', './kudu_workspace/kudu')
    if args.commit:
        execute_remote_command(args, f"cd ~/kudu_workspace/kudu && git checkout {args.commit}")
    execute_remote_command(args, "cd ~/kudu_workspace/kudu && ./docker/bootstrap-dev-env.sh && ./docker/bootstrap-java-env.sh ")
    


if __name__ == "__main__":
    main_cut()