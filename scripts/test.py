###############################################################################
# Copyright (C) 2026 Yuehao Hu, Jiatang Zhou, Tianzheng Wang, and Keval Vora
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# If you use this code or parts of it in any work (including commercial,
# open-source, academic, or non-academic projects), please cite:
# Yuehao Hu, Jiatang Zhou, Tianzheng Wang, and Keval Vora, "FARLock:
# Asymmetric RDMA Locking Made Fair," in Proceedings of the 20th USENIX
# Symposium on Operating Systems Design and Implementation (OSDI '26), 2026.
###############################################################################
import json
import subprocess
import tempfile
import os
import itertools
import argparse
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from collections import namedtuple

# --- Configuration (Edit these!) ---
EMAIL_SENDER = ""
EMAIL_PASSWORD = ""
EMAIL_RECEIVER = ""
SMTP_SERVER = "smtp.gmail.com"
SMTP_PORT = 587

# List of programs to test
PROGRAMS = {
    "../benchmark/farlock": False,
    "../benchmark/farlockg": True,
    "../benchmark/a_lock": True,
    "../benchmark/ticket_lock": True,
    "../benchmark/rdma_mcs_lock": False,
    "../benchmark/rdma_spin_lock": False,
    "../benchmark/rdma_ticket_lock": False,
}

# Path to the original JSON file
JSON_FILE_PATH = "../config/config.json"

# Parameter value ranges
THREADS_PER_NODE = [12]
# 10-30
LOCKS_PER_NODE = list(range(10, 31, 1))
LOCAL_PERCENTAGE = [100]
AVAILABLE_NODES = [10]
DISTRIBUTIONS = ["selective"]
LOCK_HOSTS = [1]

# Define critical section sizes as pairs (local, remote)
CRITICAL_SECTION_PAIRS = [
    (300, 15000),
]

# Budget pairs (only for programs that support them)
BUDGET_PAIRS = [(20, 5)]

# Number of times to repeat each test
REPEAT = 3

# Named tuple for parameters
Params = namedtuple('Params', [
    'threads_per_node', 'locks_per_node', 'local_percentage',
    'local_critical_section_size', 'remote_critical_section_size',
    'local_budget', 'remote_budget', 'available_nodes',
    'distribution', 'lock_hosts'
])

def send_email(subject, body, node_id):
    """Send an email notification only if the node ID is 0."""
    if node_id != "0":
        return
    
    if not EMAIL_PASSWORD:
        print("Email password not set. Skipping email notification.")
        return

    msg = MIMEMultipart()
    msg['From'] = EMAIL_SENDER
    msg['To'] = EMAIL_RECEIVER
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    try:
        server = smtplib.SMTP(SMTP_SERVER, SMTP_PORT)
        server.starttls()
        server.login(EMAIL_SENDER, EMAIL_PASSWORD)
        server.sendmail(EMAIL_SENDER, EMAIL_RECEIVER, msg.as_string())
        server.quit()
        print("Email notification sent successfully.")
    except Exception as e:
        print(f"Failed to send email: {e}")

def modify_json(json_data, modifications):
    """Modify JSON data with given key-value pairs."""
    for key, value in modifications.items():
        keys = key.split(".")
        d = json_data
        for k in keys[:-1]:
            if k not in d:
                d[k] = {}  # Ensure nested keys exist
            d = d[k]
        d[keys[-1]] = value
    return json_data

def get_log_filename(program, params):
    """Generate log filename based on program and selected parameters."""
    program_name = os.path.basename(program)
    base_name = f"{program_name}1_an{params.available_nodes}_d{params.distribution}"
    #base_name = f"{program_name}_ln{params.locks_per_node}_d{params.distribution}"
    #base_name = f"{program_name}_an{params.available_nodes}_ln{params.locks_per_node}_d{params.distribution}_cs{params.local_critical_section_size}-{params.remote_critical_section_size}"
    
    # Always include budgets if they exist
    if params.local_budget is not None and params.remote_budget is not None:
        base_name += f"_b{params.local_budget}-{params.remote_budget}"
    
    # Include different parameters based on distribution type
    # if params.distribution == "uniform":
    #     base_name += f"_lp{params.local_percentage}"
    # else:  # selective
    #     base_name += f"_lh{params.lock_hosts}"
    
    return f"{base_name}.log"

def main():
    # Parse command-line arguments
    parser = argparse.ArgumentParser(description="Run lock benchmarks with configurable parameters.")
    parser.add_argument("--node-id", type=str, required=True, help="Node ID for the program (e.g., '0', '1')")
    args = parser.parse_args()
    NODE_ID = args.node_id

    # Load original JSON file
    with open(JSON_FILE_PATH, "r") as f:
        json_data = json.load(f)
    
    total_tests = 0
    failed_tests = 0

    try:
        # Iterate over all parameter combinations
        for params in itertools.product(
            THREADS_PER_NODE, LOCKS_PER_NODE, LOCAL_PERCENTAGE,
            AVAILABLE_NODES, DISTRIBUTIONS, LOCK_HOSTS
        ):
            for local_cs, remote_cs in CRITICAL_SECTION_PAIRS:
                for program, supports_budget in PROGRAMS.items():
                    budget_iter = BUDGET_PAIRS if supports_budget else [(None, None)]
                    for local_budget, remote_budget in budget_iter:
                        # Create named tuple for parameters
                        param_tuple = Params(
                            threads_per_node=params[0],
                            locks_per_node=params[1],
                            local_percentage=params[2],
                            local_critical_section_size=local_cs,
                            remote_critical_section_size=remote_cs,
                            local_budget=local_budget,
                            remote_budget=remote_budget,
                            available_nodes=params[3],
                            distribution=params[4],
                            lock_hosts=params[5]
                        )

                        modifications = {
                            "benchmark.threads_per_node": param_tuple.threads_per_node,
                            "benchmark.locks_per_node": param_tuple.locks_per_node,
                            "benchmark.local_percentage": param_tuple.local_percentage,
                            "benchmark.local_critical_section_size": param_tuple.local_critical_section_size,
                            "benchmark.remote_critical_section_size": param_tuple.remote_critical_section_size,
                            "benchmark.distribution": param_tuple.distribution,
                            "benchmark.lock_hosts": param_tuple.lock_hosts,
                            "rdma.available_nodes": param_tuple.available_nodes,
                        }
                        
                        if supports_budget:
                            modifications.update({
                                "lock.local_budget": param_tuple.local_budget,
                                "lock.remote_budget": param_tuple.remote_budget,
                            })

                        modified_json = modify_json(json_data.copy(), modifications)
                        
                        with tempfile.NamedTemporaryFile(delete=False, mode="w", suffix=".json") as temp_json_file:
                            json.dump(modified_json, temp_json_file, indent=4)
                            temp_json_file.flush()  # Force write to disk
                            temp_json_file_path = temp_json_file.name
                            log_filename = get_log_filename(program, param_tuple)
                            
                            try:
                                for iteration in range(REPEAT):
                                    total_tests += 1
                                    try:
                                        result = subprocess.run(
                                            [program, NODE_ID, temp_json_file_path],
                                            check=True,
                                            capture_output=True,
                                            text=True
                                        )
                                        if NODE_ID == "0":
                                            with open(log_filename, "a") as log_file:
                                                log_file.write(
                                                    f"\nNode ID: {NODE_ID} | Config: {modifications} | Iteration: {iteration + 1}\n"
                                                )
                                                log_file.write(result.stdout + "\n")
                                                log_file.write(result.stderr + "\n")
                                                log_file.flush()
                                    except subprocess.CalledProcessError as e:
                                        failed_tests += 1
                                        if NODE_ID == "0":
                                            with open(log_filename, "a") as log_file:
                                                log_file.write(
                                                    f"\nNode ID: {NODE_ID} | Error with config {modifications} (Iteration: {iteration + 1}):\n"
                                                )
                                                log_file.write(f"STDOUT: {e.stdout}\n")
                                                log_file.write(f"STDERR: {e.stderr}\n")
                                                log_file.flush()
                                            send_email("Benchmark Failure", f"A test failed on Node {NODE_ID}. Check logs for details.", NODE_ID)
                            finally:
                                os.remove(temp_json_file_path)
    finally:
        send_email("Benchmark Script Completed", 
                  f"Benchmark script finished.\nTotal tests: {total_tests}\nFailed tests: {failed_tests}\nNode ID: {NODE_ID}", 
                  NODE_ID)

if __name__ == "__main__":
    main()