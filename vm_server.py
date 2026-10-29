import os
import paramiko
import time
import sys

# The bench VM: set BENCH_HOST, BENCH_USER and BENCH_DIR (the Entanglement
# build directory on it) in the environment.
HOST = os.environ.get('BENCH_HOST', '127.0.0.1')
USER = os.environ.get('BENCH_USER', os.environ.get('USER', ''))
BENCH_DIR = os.environ.get('BENCH_DIR', '~/Projects/Entanglement/build')

def ssh_connect(pw):
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HOST, username=USER, password=pw)
    return ssh

def run_cmd(ssh, cmd, timeout=30):
    _, stdout, stderr = ssh.exec_command(cmd, timeout=timeout)
    return stdout.read().decode(), stderr.read().decode()

def start_server(pw, workers=8):
    ssh = ssh_connect(pw)
    # Kill old
    run_cmd(ssh, 'pkill -f EntanglementNetBench')
    time.sleep(1)
    # Start in background via bash
    transport = ssh.get_transport()
    ch = transport.open_session()
    cmd = 'cd {} && ./EntanglementNetBench server 9876 {} > /tmp/bench_srv.log 2>&1 &'.format(BENCH_DIR, workers)
    ch.exec_command('bash -c ' + repr(cmd))
    ch.recv_exit_status()
    time.sleep(3)
    # Check
    out, _ = run_cmd(ssh, 'head -15 /tmp/bench_srv.log')
    print(out)
    out, _ = run_cmd(ssh, 'ss -ulnp | grep 9876')
    print('SOCKETS:', out)
    out, _ = run_cmd(ssh, 'pgrep -la EntanglementNet')
    print('PID:', out)
    ssh.close()

def check_server(pw):
    ssh = ssh_connect(pw)
    out, _ = run_cmd(ssh, 'tail -5 /tmp/bench_srv.log')
    print(out)
    out, _ = run_cmd(ssh, 'pgrep -la EntanglementNet')
    print('PID:', out)
    ssh.close()

def stop_server(pw):
    ssh = ssh_connect(pw)
    run_cmd(ssh, 'pkill -f EntanglementNetBench')
    print('Server stopped')
    ssh.close()

if __name__ == '__main__':
    pw = sys.argv[1]
    action = sys.argv[2] if len(sys.argv) > 2 else 'start'
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    if action == 'start':
        start_server(pw, workers)
    elif action == 'check':
        check_server(pw)
    elif action == 'stop':
        stop_server(pw)
