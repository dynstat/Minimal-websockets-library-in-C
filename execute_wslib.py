import subprocess
import os
import glob

def run_command(command):
    print(f"Executing: {command}")
    result = subprocess.run(command, shell=True, capture_output=True, text=True)
    print(result.stdout)
    if result.stderr:
        print("Error:", result.stderr)
    if result.returncode != 0:
        print(f"Command failed with return code {result.returncode}")
        exit(1)

def find_vcvars64():
    program_files = os.environ.get('ProgramFiles(x86)', 'C:\\Program Files (x86)')
    vs_path = os.path.join(program_files, 'Microsoft Visual Studio')
    
    if not os.path.exists(vs_path):
        print("Microsoft Visual Studio folder not found.")
        return None

    # Search for all vcvars64.bat files
    vcvars_paths = glob.glob(os.path.join(vs_path, '*', '*', 'VC', 'Auxiliary', 'Build', 'vcvars64.bat'))
    
    if not vcvars_paths:
        print("vcvars64.bat not found. Please check the Visual Studio installation.")
        return None

    # Return the first found vcvars64.bat
    return vcvars_paths[0]

def main():
    vcvars_path = find_vcvars64()
    if not vcvars_path:
        return

    print(f"Using vcvars64.bat from: {vcvars_path}")

    # Compile ws_lib.c
    run_command(f'"{vcvars_path}" && cl /c ws_lib.c')

    # Create library
    run_command(f'"{vcvars_path}" && lib ws_lib.obj')

    # Compile example_usage.c
    run_command(f'"{vcvars_path}" && cl example_usage.c ws_lib.lib')

    # Run the example program
    # run_command('example_usage.exe')

if __name__ == "__main__":
    main()