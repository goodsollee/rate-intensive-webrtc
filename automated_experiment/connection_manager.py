import subprocess
import random
import string
import os
import signal
import json
from flask import Flask, request, jsonify
import logging
from typing import Dict

app = Flask(__name__)

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class ConnectionManager:
    def __init__(self):
        self.active_connections: Dict[str, subprocess.Popen] = {}
        self.client_executable = "../out/Default/peerconnection_client"  # Changed to relative path in same folder

    def generate_room_number(self) -> str:
        """Generate an 8-digit random room number."""
        return ''.join(random.choices(string.digits, k=8))

    def start_client(self, url: str, room_number: str = None, autoconnect: bool = True) -> bool:
        """Start a new peerconnection_client process with specified parameters."""
        try:
            if not os.path.exists(self.client_executable):
                logger.error(f"Client executable not found at: {self.client_executable}")
                return False

            # Build command with parameters
            cmd = [self.client_executable, f"--server={url}"]
            
            # Add autoconnect flag if enabled
            if autoconnect:
                cmd.append("--autoconnect")
            
            # Add room_id if provided
            if room_number:
                cmd.append(f"--room_id={room_number}")
            else:
                # Generate random room number if not provided
                room_number = self.generate_room_number()
                cmd.append(f"--room_id={room_number}")
            
            logger.info(f"Starting client with command: {' '.join(cmd)}")
            # Set working directory to where the executable is
            executable_dir = os.path.dirname(os.path.abspath("../out/Default/"))
            
            '''
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=executable_dir,  # Set working directory
                env=os.environ.copy()  # Pass current environment
            )
            '''

            # Add logging for debugging
            '''
            def log_output(pipe, prefix):
                for line in iter(pipe.readline, b''):
                    logger.info(f"{prefix}: {line.decode().strip()}")
                    
            import threading
            #threading.Thread(target=log_output, args=(process.stdout, "STDOUT")).start()
            threading.Thread(target=log_output, args=(process.stderr, "STDERR")).start()
            '''

            process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,  # Ignore stdout
                stderr=subprocess.DEVNULL,  # Ignore stderr
                cwd=executable_dir, 
                env=os.environ.copy()
            )
    
            return True
            
        except Exception as e:
            logger.error(f"Error starting client: {str(e)}")
            return False

    def stop_client(self, room_number: str) -> bool:
        """Stop a running peerconnection_client process."""
        if room_number not in self.active_connections:
            logger.warning(f"No active connection found for room {room_number}")
            return False

        try:
            process = self.active_connections[room_number]
            process.terminate()
            process.wait(timeout=5)  # Wait up to 5 seconds for graceful termination
            
            # Force kill if still running
            if process.poll() is None:
                process.kill()
                process.wait()

            del self.active_connections[room_number]
            logger.info(f"Stopped client for room {room_number}")
            return True

        except Exception as e:
            logger.error(f"Error stopping client: {str(e)}")
            return False

    def get_active_connections(self) -> list:
        """Get list of active room numbers."""
        return list(self.active_connections.keys())

# Create connection manager instance
connection_mgr = ConnectionManager()

@app.route('/start', methods=['POST'])
def start_connection():
    try:
        data = request.get_json()
        if not data or 'url' not in data:
            return jsonify({'error': 'URL parameter is required'}), 400

        url = data['url']
        room_number = data.get('room_number')  # Optional room number
        autoconnect = data.get('autoconnect', True)  # Optional autoconnect flag, defaults to True

        if connection_mgr.start_client(url, room_number, autoconnect):
            return jsonify({
                'status': 'success',
                'room_number': room_number,
                'message': 'Connection started successfully'
            })
        else:
            return jsonify({'error': 'Failed to start connection'}), 500

    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/stop', methods=['POST'])
def stop_connection():
    try:
        data = request.get_json()
        if not data or 'room_number' not in data:
            return jsonify({'error': 'Room number is required'}), 400

        room_number = data['room_number']
        if connection_mgr.stop_client(room_number):
            return jsonify({
                'status': 'success',
                'message': f'Connection {room_number} stopped successfully'
            })
        else:
            return jsonify({'error': 'Failed to stop connection'}), 404

    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/status', methods=['GET'])
def get_status():
    try:
        active_connections = connection_mgr.get_active_connections()
        return jsonify({
            'status': 'success',
            'active_connections': active_connections,
            'count': len(active_connections)
        })

    except Exception as e:
        return jsonify({'error': str(e)}), 500

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)