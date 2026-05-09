"""
Unreal Engine Advanced MCP Server

A streamlined MCP server focused on advanced composition tools for Unreal Engine.
Contains only the advanced tools from the expanded MCP tool system to keep tool count manageable.
"""

import logging
import socket
import json
import math
import struct
import time
import threading
from contextlib import asynccontextmanager
from typing import AsyncIterator, Dict, Any, Optional, List, Union
from mcp.server.fastmcp import FastMCP

from helpers.infrastructure_creation import (
    _create_street_grid, _create_street_lights, _create_town_vehicles, _create_town_decorations,
    _create_traffic_lights, _create_street_signage, _create_sidewalks_crosswalks, _create_urban_furniture,
    _create_street_utilities, _create_central_plaza
)
from helpers.building_creation import _create_town_building
from helpers.castle_creation import (
    get_castle_size_params, calculate_scaled_dimensions, build_outer_bailey_walls, 
    build_inner_bailey_walls, build_gate_complex, build_corner_towers, 
    build_inner_corner_towers, build_intermediate_towers, build_central_keep, 
    build_courtyard_complex, build_bailey_annexes, build_siege_weapons, 
    build_village_settlement, build_drawbridge_and_moat, add_decorative_flags
)
from helpers.house_construction import build_house

from helpers.mansion_creation import (
    get_mansion_size_params, calculate_mansion_layout, build_mansion_main_structure,
    build_mansion_exterior, add_mansion_interior
)
from helpers.actor_utilities import spawn_blueprint_actor, get_blueprint_material_info
from helpers.actor_name_manager import (
    safe_spawn_actor, safe_delete_actor
)
from helpers.bridge_aqueduct_creation import (
    build_suspension_bridge_structure, build_aqueduct_structure
)

# ============================================================================
# Blueprint Node Graph Tools
# ============================================================================
from helpers.blueprint_graph import node_manager
from helpers.blueprint_graph import variable_manager
from helpers.blueprint_graph import connector_manager
from helpers.blueprint_graph import event_manager
from helpers.blueprint_graph import node_deleter
from helpers.blueprint_graph import node_properties
from helpers.blueprint_graph import function_manager
from helpers.blueprint_graph import function_io


# Configure logging with more detailed format
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s - %(name)s - %(levelname)s - [%(filename)s:%(lineno)d] - %(message)s',
    handlers=[
        logging.FileHandler('unreal_mcp_advanced.log'),
    ]
)
logger = logging.getLogger("UnrealMCP_Advanced")

# Configuration
UNREAL_HOST = "127.0.0.1"
UNREAL_PORT = 55557

class UnrealConnection:
    """
    Robust connection to Unreal Engine with automatic retry and reconnection.
    
    Features:
    - Exponential backoff retry for connection attempts
    - Automatic reconnection on failure
    - Configurable timeouts per command type
    - Thread-safe operations
    - Detailed logging for debugging
    """
    
    # Configuration constants
    MAX_RETRIES = 3
    BASE_RETRY_DELAY = 0.5  # seconds
    MAX_RETRY_DELAY = 5.0   # seconds
    CONNECT_TIMEOUT = 10    # seconds
    DEFAULT_RECV_TIMEOUT = 30  # seconds
    LARGE_OP_RECV_TIMEOUT = 300  # seconds for large operations
    BUFFER_SIZE = 8192
    
    # Commands that need longer timeouts
    LARGE_OPERATION_COMMANDS = {
        "get_available_materials",
        "create_town",
        "create_castle_fortress", 
        "construct_mansion",
        "create_suspension_bridge",
        "create_aqueduct",
        "create_maze"
    }
    
    def __init__(self):
        """Initialize the connection."""
        self.socket = None
        self.connected = False
        self._lock = threading.RLock()  # RLock allows reentrant acquisition for retry logic
        self._last_error = None
    
    def _create_socket(self) -> socket.socket:
        """Create and configure a new socket."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.CONNECT_TIMEOUT)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 131072)  # 128KB
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 131072)  # 128KB
        
        # Set linger to ensure clean socket closure (l_onoff=1, l_linger=0)
        # struct linger is two 16-bit integers: l_onoff and l_linger
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('hh', 1, 0))
        except OSError:
            pass
        
        return sock
    
    def connect(self) -> bool:
        """
        Connect to Unreal Engine with retry logic.
        
        Uses exponential backoff for retries. Sleep occurs outside the lock
        to avoid blocking other threads during retry delays.
            
        Returns:
            True if connected successfully, False otherwise
        """
        for attempt in range(self.MAX_RETRIES + 1):
            # Hold lock only during connection attempt, not during sleep
            with self._lock:
                # Clean up any existing connection
                self._close_socket_unsafe()
                
                try:
                    logger.info(f"Connecting to Unreal at {UNREAL_HOST}:{UNREAL_PORT} (attempt {attempt + 1}/{self.MAX_RETRIES + 1})...")
                    
                    self.socket = self._create_socket()
                    self.socket.connect((UNREAL_HOST, UNREAL_PORT))
                    self.connected = True
                    self._last_error = None
                    
                    logger.info("Successfully connected to Unreal Engine")
                    return True
                    
                except socket.timeout as e:
                    self._last_error = f"Connection timeout: {e}"
                    logger.warning(f"Connection timeout (attempt {attempt + 1})")
                except ConnectionRefusedError as e:
                    self._last_error = f"Connection refused: {e}"
                    logger.warning(f"Connection refused - is Unreal Engine running? (attempt {attempt + 1})")
                except OSError as e:
                    self._last_error = f"OS error: {e}"
                    logger.warning(f"OS error during connection: {e} (attempt {attempt + 1})")
                except Exception as e:
                    self._last_error = f"Unexpected error: {e}"
                    logger.error(f"Unexpected connection error: {e} (attempt {attempt + 1})")
                
                self._close_socket_unsafe()
                self.connected = False
            
            # Sleep OUTSIDE the lock to allow other threads to proceed
            if attempt < self.MAX_RETRIES:
                delay = min(self.BASE_RETRY_DELAY * (2 ** attempt), self.MAX_RETRY_DELAY)
                logger.info(f"Retrying connection in {delay:.1f}s...")
                time.sleep(delay)
        
        logger.error(f"Failed to connect after {self.MAX_RETRIES + 1} attempts. Last error: {self._last_error}")
        return False
    
    def _close_socket_unsafe(self):
        """Close socket without lock (internal use only)."""
        if self.socket:
            try:
                self.socket.shutdown(socket.SHUT_RDWR)
            except:
                pass
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
        self.connected = False
    
    def disconnect(self):
        """Safely disconnect from Unreal Engine."""
        with self._lock:
            self._close_socket_unsafe()
            logger.debug("Disconnected from Unreal Engine")

    def _get_timeout_for_command(self, command_type: str) -> int:
        """Get appropriate timeout for command type."""
        if any(large_cmd in command_type for large_cmd in self.LARGE_OPERATION_COMMANDS):
            return self.LARGE_OP_RECV_TIMEOUT
        return self.DEFAULT_RECV_TIMEOUT

    def _receive_response(self, command_type: str) -> bytes:
        """
        Receive complete JSON response from Unreal.
        
        Args:
            command_type: Type of command (used for timeout selection)
            
        Returns:
            Raw response bytes
            
        Raises:
            Exception: On timeout or connection error
        """
        timeout = self._get_timeout_for_command(command_type)
        self.socket.settimeout(timeout)
        
        chunks = []
        total_bytes = 0
        start_time = time.time()
        
        try:
            while True:
                # Check for overall timeout
                elapsed = time.time() - start_time
                if elapsed > timeout:
                    raise socket.timeout(f"Overall timeout after {elapsed:.1f}s")
                
                try:
                    chunk = self.socket.recv(self.BUFFER_SIZE)
                except socket.timeout:
                    # Check if we have a complete response
                    if chunks:
                        data = b''.join(chunks)
                        try:
                            json.loads(data.decode('utf-8'))
                            logger.info(f"Got complete response after recv timeout ({total_bytes} bytes)")
                            return data
                        except json.JSONDecodeError:
                            pass
                    raise
                
                if not chunk:
                    # Connection closed by remote
                    if not chunks:
                        raise ConnectionError("Connection closed before receiving any data")
                    break
                
                chunks.append(chunk)
                total_bytes += len(chunk)
                
                # Try to parse accumulated data as JSON
                data = b''.join(chunks)
                try:
                    decoded = data.decode('utf-8')
                    json.loads(decoded)
                    # Successfully parsed - we have complete response
                    logger.info(f"Received complete response ({total_bytes} bytes) for {command_type}")
                    return data
                except json.JSONDecodeError:
                    # Incomplete JSON, continue reading
                    continue
                except UnicodeDecodeError:
                    # Incomplete UTF-8, continue reading
                    continue
                    
        except socket.timeout:
            elapsed = time.time() - start_time
            if chunks:
                data = b''.join(chunks)
                try:
                    json.loads(data.decode('utf-8'))
                    logger.warning(f"Using response received before timeout ({total_bytes} bytes)")
                    return data
                except:
                    pass
            raise TimeoutError(f"Timeout after {elapsed:.1f}s waiting for response to {command_type} (received {total_bytes} bytes)")
        
        # If we get here, connection was closed
        if chunks:
            data = b''.join(chunks)
            try:
                json.loads(data.decode('utf-8'))
                return data
            except:
                raise ConnectionError(f"Connection closed with incomplete data ({total_bytes} bytes)")
        
        raise ConnectionError("Connection closed without response")

    def send_command(self, command: str, params: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        """
        Send a command to Unreal Engine with automatic retry.
        
        Args:
            command: Command type string
            params: Command parameters dictionary
            
        Returns:
            Response dictionary or error dictionary
        """
        last_error = None
        
        for attempt in range(self.MAX_RETRIES + 1):
            try:
                return self._send_command_once(command, params, attempt)
            except (ConnectionError, TimeoutError, socket.error, OSError) as e:
                last_error = str(e)
                logger.warning(f"Command failed (attempt {attempt + 1}/{self.MAX_RETRIES + 1}): {e}")
                
                # Clean up and prepare for retry
                self.disconnect()
                
                if attempt < self.MAX_RETRIES:
                    delay = min(self.BASE_RETRY_DELAY * (2 ** attempt), self.MAX_RETRY_DELAY)
                    logger.info(f"Retrying command in {delay:.1f}s...")
                    time.sleep(delay)
            except Exception as e:
                # Unexpected error - don't retry
                logger.error(f"Unexpected error sending command: {e}")
                self.disconnect()
                return {"status": "error", "error": str(e)}
        
        return {"status": "error", "error": f"Command failed after {self.MAX_RETRIES + 1} attempts: {last_error}"}

    def _send_command_once(self, command: str, params: Dict[str, Any], attempt: int) -> Dict[str, Any]:
        """
        Send command once (internal method).
        
        Args:
            command: Command type
            params: Command parameters
            attempt: Current attempt number
            
        Returns:
            Response dictionary
            
        Raises:
            Various exceptions on failure
        """
        # Hold lock for entire send-receive cycle to prevent race conditions
        # where another thread could close/reconnect the socket mid-operation.
        # RLock allows nested acquisition from connect()/disconnect() calls.
        with self._lock:
            # Connect (or reconnect)
            if not self.connect():
                raise ConnectionError(f"Failed to connect to Unreal Engine: {self._last_error}")
            
            try:
                # Build and send command
                command_obj = {
                    "type": command,
                    "params": params or {}
                }
                command_json = json.dumps(command_obj)
                
                logger.info(f"Sending command (attempt {attempt + 1}): {command}")
                logger.debug(f"Command payload: {command_json[:500]}...")
                
                # Send with timeout
                self.socket.settimeout(10)  # 10 second send timeout
                self.socket.sendall(command_json.encode('utf-8'))
                
                # Receive response
                response_data = self._receive_response(command)
                
                # Parse response
                try:
                    response = json.loads(response_data.decode('utf-8'))
                except json.JSONDecodeError as e:
                    logger.error(f"JSON decode error: {e}")
                    logger.debug(f"Raw response: {response_data[:500]}")
                    raise ValueError(f"Invalid JSON response: {e}")
                
                logger.info(f"Command {command} completed successfully")
                
                # Normalize error responses
                if response.get("status") == "error":
                    error_msg = response.get("error") or response.get("message", "Unknown error")
                    logger.warning(f"Unreal returned error: {error_msg}")
                elif response.get("success") is False:
                    error_msg = response.get("error") or response.get("message", "Unknown error")
                    response = {"status": "error", "error": error_msg}
                    logger.warning(f"Unreal returned failure: {error_msg}")
                
                return response
                
            finally:
                # Always clean up connection after command
                self._close_socket_unsafe()

# Global connection instance (singleton pattern)
_unreal_connection: Optional[UnrealConnection] = None
_connection_lock = threading.Lock()

def get_unreal_connection() -> UnrealConnection:
    """
    Get the global Unreal connection instance.
    
    Uses lazy initialization - connection is created on first access.
    The connection handles its own retry logic, so we don't need to
    pre-connect here.
    
    Returns:
        UnrealConnection instance (always returns an instance, never None)
    """
    global _unreal_connection
    
    with _connection_lock:
        if _unreal_connection is None:
            logger.info("Creating new UnrealConnection instance")
            _unreal_connection = UnrealConnection()
        return _unreal_connection


def reset_unreal_connection():
    """Reset the global connection (useful for error recovery)."""
    global _unreal_connection
    
    with _connection_lock:
        if _unreal_connection:
            _unreal_connection.disconnect()
            _unreal_connection = None
        logger.info("Unreal connection reset")

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    logger.info("UnrealMCP Advanced server starting up")
    logger.info("Connection will be established lazily on first tool call")

    try:
        yield {}
    finally:
        reset_unreal_connection()
        logger.info("Unreal MCP Advanced server shut down")

# Initialize server
mcp = FastMCP(
    "UnrealMCP_Advanced",
    lifespan=server_lifespan
)

# Essential Actor Management Tools
@mcp.tool()
def get_actors_in_level(random_string: str = "") -> Dict[str, Any]:
    """Get a list of all actors in the current level."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        response = unreal.send_command("get_actors_in_level", {})
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"get_actors_in_level error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def find_actors_by_name(pattern: str) -> Dict[str, Any]:
    """Find actors by name pattern."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        response = unreal.send_command("find_actors_by_name", {"pattern": pattern})
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"find_actors_by_name error: {e}")
        return {"success": False, "message": str(e)}



@mcp.tool()
def delete_actor(name: str) -> Dict[str, Any]:
    """Delete an actor by name."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        # Use the safe delete function to update tracking
        response = safe_delete_actor(unreal, name)
        return response
    except Exception as e:
        logger.error(f"delete_actor error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def set_actor_transform(
    name: str,
    location: List[float] = None,
    rotation: List[float] = None,
    scale: List[float] = None
) -> Dict[str, Any]:
    """Set the transform of an actor."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {"name": name}
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if scale is not None:
            params["scale"] = scale
            
        response = unreal.send_command("set_actor_transform", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"set_actor_transform error: {e}")
        return {"success": False, "message": str(e)}

# Essential Blueprint Tools for Physics Actors
@mcp.tool()
def create_blueprint(name: str, parent_class: str) -> Dict[str, Any]:
    """Create a new Blueprint class."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "name": name,
            "parent_class": parent_class
        }
        response = unreal.send_command("create_blueprint", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"create_blueprint error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def add_component_to_blueprint(
    blueprint_name: str,
    component_type: str,
    component_name: str,
    location: List[float] = [],
    rotation: List[float] = [],
    scale: List[float] = [],
    component_properties: Dict[str, Any] = {}
) -> Dict[str, Any]:
    """Add a component to a Blueprint."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_name": blueprint_name,
            "component_type": component_type,
            "component_name": component_name,
            "location": location,
            "rotation": rotation,
            "scale": scale,
            "component_properties": component_properties
        }
        response = unreal.send_command("add_component_to_blueprint", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"add_component_to_blueprint error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def set_static_mesh_properties(
    blueprint_name: str,
    component_name: str,
    static_mesh: str = "/Engine/BasicShapes/Cube.Cube"
) -> Dict[str, Any]:
    """Set static mesh properties on a StaticMeshComponent."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "static_mesh": static_mesh
        }
        response = unreal.send_command("set_static_mesh_properties", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"set_static_mesh_properties error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def set_physics_properties(
    blueprint_name: str,
    component_name: str,
    simulate_physics: bool = True,
    gravity_enabled: bool = True,
    mass: float = 1,
    linear_damping: float = 0.01,
    angular_damping: float = 0
) -> Dict[str, Any]:
    """Set physics properties on a component."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "simulate_physics": simulate_physics,
            "gravity_enabled": gravity_enabled,
            "mass": mass,
            "linear_damping": linear_damping,
            "angular_damping": angular_damping
        }
        response = unreal.send_command("set_physics_properties", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"set_physics_properties error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def compile_blueprint(blueprint_name: str) -> Dict[str, Any]:
    """Compile a Blueprint."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {"blueprint_name": blueprint_name}
        response = unreal.send_command("compile_blueprint", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"compile_blueprint error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def read_blueprint_content(
    blueprint_path: str,
    include_event_graph: bool = True,
    include_functions: bool = True,
    include_variables: bool = True,
    include_components: bool = True,
    include_interfaces: bool = True
) -> Dict[str, Any]:
    """
    Read and analyze the complete content of a Blueprint including event graph, 
    functions, variables, components, and implemented interfaces.
    
    Args:
        blueprint_path: Full path to the Blueprint asset (e.g., "/Game/MyBlueprint.MyBlueprint")
        include_event_graph: Include event graph nodes and connections
        include_functions: Include custom functions and their graphs
        include_variables: Include all Blueprint variables with types and defaults
        include_components: Include component hierarchy and properties
        include_interfaces: Include implemented Blueprint interfaces
    
    Returns:
        Dictionary containing complete Blueprint structure and content
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_path": blueprint_path,
            "include_event_graph": include_event_graph,
            "include_functions": include_functions,
            "include_variables": include_variables,
            "include_components": include_components,
            "include_interfaces": include_interfaces
        }
        
        logger.info(f"Reading Blueprint content for: {blueprint_path}")
        response = unreal.send_command("read_blueprint_content", params)
        
        if response and response.get("success", False):
            logger.info(f"Successfully read Blueprint content. Found:")
            if response.get("variables"):
                logger.info(f"  - {len(response['variables'])} variables")
            if response.get("functions"):
                logger.info(f"  - {len(response['functions'])} functions")
            if response.get("event_graph", {}).get("nodes"):
                logger.info(f"  - {len(response['event_graph']['nodes'])} event graph nodes")
            if response.get("components"):
                logger.info(f"  - {len(response['components'])} components")
        
        return response or {"success": False, "message": "No response from Unreal"}
        
    except Exception as e:
        logger.error(f"read_blueprint_content error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def analyze_blueprint_graph(
    blueprint_path: str,
    graph_name: str = "EventGraph",
    include_node_details: bool = True,
    include_pin_connections: bool = True,
    trace_execution_flow: bool = True
) -> Dict[str, Any]:
    """
    Analyze a specific graph within a Blueprint (EventGraph, functions, etc.)
    and provide detailed information about nodes, connections, and execution flow.
    
    Args:
        blueprint_path: Full path to the Blueprint asset
        graph_name: Name of the graph to analyze ("EventGraph", function name, etc.)
        include_node_details: Include detailed node properties and settings
        include_pin_connections: Include all pin-to-pin connections
        trace_execution_flow: Trace the execution flow through the graph
    
    Returns:
        Dictionary with graph analysis including nodes, connections, and flow
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_path": blueprint_path,
            "graph_name": graph_name,
            "include_node_details": include_node_details,
            "include_pin_connections": include_pin_connections,
            "trace_execution_flow": trace_execution_flow
        }
        
        logger.info(f"Analyzing Blueprint graph: {blueprint_path} -> {graph_name}")
        response = unreal.send_command("analyze_blueprint_graph", params)
        
        if response and response.get("success", False):
            graph_data = response.get("graph_data", {})
            logger.info(f"Graph analysis complete:")
            logger.info(f"  - Graph: {graph_data.get('graph_name', 'Unknown')}")
            logger.info(f"  - Nodes: {len(graph_data.get('nodes', []))}")
            logger.info(f"  - Connections: {len(graph_data.get('connections', []))}")
            if graph_data.get('execution_paths'):
                logger.info(f"  - Execution paths: {len(graph_data['execution_paths'])}")
        
        return response or {"success": False, "message": "No response from Unreal"}
        
    except Exception as e:
        logger.error(f"analyze_blueprint_graph error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def get_blueprint_variable_details(
    blueprint_path: str,
    variable_name: str = None
) -> Dict[str, Any]:
    """
    Get detailed information about Blueprint variables including type, 
    default values, metadata, and usage within the Blueprint.
    
    Args:
        blueprint_path: Full path to the Blueprint asset
        variable_name: Specific variable name (if None, returns all variables)
    
    Returns:
        Dictionary with variable details including type, defaults, and usage
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_path": blueprint_path,
            "variable_name": variable_name
        }
        
        logger.info(f"Getting Blueprint variable details: {blueprint_path}")
        if variable_name:
            logger.info(f"  - Specific variable: {variable_name}")
        
        response = unreal.send_command("get_blueprint_variable_details", params)
        return response or {"success": False, "message": "No response from Unreal"}
        
    except Exception as e:
        logger.error(f"get_blueprint_variable_details error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def get_blueprint_function_details(
    blueprint_path: str,
    function_name: str = None,
    include_graph: bool = True
) -> Dict[str, Any]:
    """
    Get detailed information about Blueprint functions including parameters,
    return values, local variables, and function graph content.
    
    Args:
        blueprint_path: Full path to the Blueprint asset
        function_name: Specific function name (if None, returns all functions)
        include_graph: Include the function's graph nodes and connections
    
    Returns:
        Dictionary with function details including signature and graph content
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_path": blueprint_path,
            "function_name": function_name,
            "include_graph": include_graph
        }
        
        logger.info(f"Getting Blueprint function details: {blueprint_path}")
        if function_name:
            logger.info(f"  - Specific function: {function_name}")
        
        response = unreal.send_command("get_blueprint_function_details", params)
        return response or {"success": False, "message": "No response from Unreal"}
        
    except Exception as e:
        logger.error(f"get_blueprint_function_details error: {e}")
        return {"success": False, "message": str(e)}



# Advanced Composition Tools
@mcp.tool()
def create_pyramid(
    base_size: int = 3,
    block_size: float = 100.0,
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "PyramidBlock",
    mesh: str = "/Engine/BasicShapes/Cube.Cube"
) -> Dict[str, Any]:
    """Spawn a pyramid made of cube actors."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        spawned = []
        scale = block_size / 100.0
        for level in range(base_size):
            count = base_size - level
            for x in range(count):
                for y in range(count):
                    actor_name = f"{name_prefix}_{level}_{x}_{y}"
                    loc = [
                        location[0] + (x - (count - 1)/2) * block_size,
                        location[1] + (y - (count - 1)/2) * block_size,
                        location[2] + level * block_size
                    ]
                    params = {
                        "name": actor_name,
                        "type": "StaticMeshActor",
                        "location": loc,
                        "scale": [scale, scale, scale],
                        "static_mesh": mesh
                    }
                    resp = safe_spawn_actor(unreal, params)
                    if resp and resp.get("status") == "success":
                        spawned.append(resp)
        return {"success": True, "actors": spawned}
    except Exception as e:
        logger.error(f"create_pyramid error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_wall(
    length: int = 5,
    height: int = 2,
    block_size: float = 100.0,
    location: List[float] = [0.0, 0.0, 0.0],
    orientation: str = "x",
    name_prefix: str = "WallBlock",
    mesh: str = "/Engine/BasicShapes/Cube.Cube"
) -> Dict[str, Any]:
    """Create a simple wall from cubes."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        spawned = []
        scale = block_size / 100.0
        for h in range(height):
            for i in range(length):
                actor_name = f"{name_prefix}_{h}_{i}"
                if orientation == "x":
                    loc = [location[0] + i * block_size, location[1], location[2] + h * block_size]
                else:
                    loc = [location[0], location[1] + i * block_size, location[2] + h * block_size]
                params = {
                    "name": actor_name,
                    "type": "StaticMeshActor",
                    "location": loc,
                    "scale": [scale, scale, scale],
                    "static_mesh": mesh
                }
                resp = safe_spawn_actor(unreal, params)
                if resp and resp.get("status") == "success":
                    spawned.append(resp)
        return {"success": True, "actors": spawned}
    except Exception as e:
        logger.error(f"create_wall error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_tower(
    height: int = 10,
    base_size: int = 4,
    block_size: float = 100.0,
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "TowerBlock",
    mesh: str = "/Engine/BasicShapes/Cube.Cube",
    tower_style: str = "cylindrical"  # "cylindrical", "square", "tapered"
) -> Dict[str, Any]:
    """Create a realistic tower with various architectural styles."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        spawned = []
        scale = block_size / 100.0

        for level in range(height):
            level_height = location[2] + level * block_size
            
            if tower_style == "cylindrical":
                # Create circular tower
                radius = (base_size / 2) * block_size  # Convert to world units (centimeters)
                circumference = 2 * math.pi * radius
                num_blocks = max(8, int(circumference / block_size))
                
                for i in range(num_blocks):
                    angle = (2 * math.pi * i) / num_blocks
                    x = location[0] + radius * math.cos(angle)
                    y = location[1] + radius * math.sin(angle)
                    
                    actor_name = f"{name_prefix}_{level}_{i}"
                    params = {
                        "name": actor_name,
                        "type": "StaticMeshActor",
                        "location": [x, y, level_height],
                        "scale": [scale, scale, scale],
                        "static_mesh": mesh
                    }
                    resp = safe_spawn_actor(unreal, params)
                    if resp and resp.get("status") == "success":
                        spawned.append(resp)
                        
            elif tower_style == "tapered":
                # Create tapering square tower
                current_size = max(1, base_size - (level // 2))
                half_size = current_size / 2
                
                # Create walls for current level
                for side in range(4):
                    for i in range(current_size):
                        if side == 0:  # Front wall
                            x = location[0] + (i - half_size + 0.5) * block_size
                            y = location[1] - half_size * block_size
                            actor_name = f"{name_prefix}_{level}_front_{i}"
                        elif side == 1:  # Right wall
                            x = location[0] + half_size * block_size
                            y = location[1] + (i - half_size + 0.5) * block_size
                            actor_name = f"{name_prefix}_{level}_right_{i}"
                        elif side == 2:  # Back wall
                            x = location[0] + (half_size - i - 0.5) * block_size
                            y = location[1] + half_size * block_size
                            actor_name = f"{name_prefix}_{level}_back_{i}"
                        else:  # Left wall
                            x = location[0] - half_size * block_size
                            y = location[1] + (half_size - i - 0.5) * block_size
                            actor_name = f"{name_prefix}_{level}_left_{i}"
                            
                        params = {
                            "name": actor_name,
                            "type": "StaticMeshActor",
                            "location": [x, y, level_height],
                            "scale": [scale, scale, scale],
                            "static_mesh": mesh
                        }
                        resp = unreal.send_command("spawn_actor", params)
                        if resp:
                            spawned.append(resp)
                            
            else:  # square tower
                # Create square tower walls
                half_size = base_size / 2
                
                # Four walls
                for side in range(4):
                    for i in range(base_size):
                        if side == 0:  # Front wall
                            x = location[0] + (i - half_size + 0.5) * block_size
                            y = location[1] - half_size * block_size
                            actor_name = f"{name_prefix}_{level}_front_{i}"
                        elif side == 1:  # Right wall
                            x = location[0] + half_size * block_size
                            y = location[1] + (i - half_size + 0.5) * block_size
                            actor_name = f"{name_prefix}_{level}_right_{i}"
                        elif side == 2:  # Back wall
                            x = location[0] + (half_size - i - 0.5) * block_size
                            y = location[1] + half_size * block_size
                            actor_name = f"{name_prefix}_{level}_back_{i}"
                        else:  # Left wall
                            x = location[0] - half_size * block_size
                            y = location[1] + (half_size - i - 0.5) * block_size
                            actor_name = f"{name_prefix}_{level}_left_{i}"
                            
                        params = {
                            "name": actor_name,
                            "type": "StaticMeshActor",
                            "location": [x, y, level_height],
                            "scale": [scale, scale, scale],
                            "static_mesh": mesh
                        }
                        resp = unreal.send_command("spawn_actor", params)
                        if resp:
                            spawned.append(resp)
                            
            # Add decorative elements every few levels
            if level % 3 == 2 and level < height - 1:
                # Add corner details
                for corner in range(4):
                    angle = corner * math.pi / 2
                    detail_x = location[0] + (base_size/2 + 0.5) * block_size * math.cos(angle)
                    detail_y = location[1] + (base_size/2 + 0.5) * block_size * math.sin(angle)
                    
                    actor_name = f"{name_prefix}_{level}_detail_{corner}"
                    params = {
                        "name": actor_name,
                        "type": "StaticMeshActor",
                        "location": [detail_x, detail_y, level_height],
                        "scale": [scale * 0.7, scale * 0.7, scale * 0.7],
                        "static_mesh": "/Engine/BasicShapes/Cylinder.Cylinder"
                    }
                    resp = safe_spawn_actor(unreal, params)
                    if resp and resp.get("status") == "success":
                        spawned.append(resp)
                        
        return {"success": True, "actors": spawned, "tower_style": tower_style}
    except Exception as e:
        logger.error(f"create_tower error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_staircase(
    steps: int = 5,
    step_size: List[float] = [100.0, 100.0, 50.0],
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "Stair",
    mesh: str = "/Engine/BasicShapes/Cube.Cube"
) -> Dict[str, Any]:
    """Create a staircase from cubes."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        spawned = []
        sx, sy, sz = step_size
        for i in range(steps):
            actor_name = f"{name_prefix}_{i}"
            loc = [location[0] + i * sx, location[1], location[2] + i * sz]
            scale = [sx/100.0, sy/100.0, sz/100.0]
            params = {
                "name": actor_name,
                "type": "StaticMeshActor",
                "location": loc,
                "scale": scale,
                "static_mesh": mesh
            }
            resp = safe_spawn_actor(unreal, params)
            if resp and resp.get("status") == "success":
                spawned.append(resp)
        return {"success": True, "actors": spawned}
    except Exception as e:
        logger.error(f"create_staircase error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def construct_house(
    width: int = 1200,
    depth: int = 1000,
    height: int = 600,
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "House",
    mesh: str = "/Engine/BasicShapes/Cube.Cube",
    house_style: str = "modern"  # "modern", "cottage"
) -> Dict[str, Any]:
    """Construct a realistic house with architectural details and multiple rooms."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}

        # Use the helper function to build the house
        return build_house(unreal, width, depth, height, location, name_prefix, mesh, house_style)

    except Exception as e:
        logger.error(f"construct_house error: {e}")
        return {"success": False, "message": str(e)}



@mcp.tool()
def construct_mansion(
    mansion_scale: str = "large",  # "small", "large", "epic", "legendary"
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "Mansion"
) -> Dict[str, Any]:
    """
    Construct a magnificent mansion with multiple wings, grand rooms, gardens,
    fountains, and luxury features perfect for dramatic TikTok reveals.
    """
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}

        logger.info(f"Creating {mansion_scale} mansion")
        all_actors = []

        # Get size parameters and calculate scaled dimensions
        params = get_mansion_size_params(mansion_scale)
        layout = calculate_mansion_layout(params)

        # Build mansion main structure
        build_mansion_main_structure(unreal, name_prefix, location, layout, all_actors)

        # Build mansion exterior
        build_mansion_exterior(unreal, name_prefix, location, layout, all_actors)

        # Add luxurious interior
        add_mansion_interior(unreal, name_prefix, location, layout, all_actors)

        logger.info(f"Mansion construction complete! Created {len(all_actors)} elements")

        return {
            "success": True,
            "message": f"Magnificent {mansion_scale} mansion created with {len(all_actors)} elements!",
            "actors": all_actors,
            "stats": {
                "scale": mansion_scale,
                "wings": layout["wings"],
                "floors": layout["floors"],
                "main_rooms": layout["main_rooms"],
                "bedrooms": layout["bedrooms"],
                "garden_size": layout["garden_size"],
                "fountain_count": layout["fountain_count"],
                "car_count": layout["car_count"],
                "total_actors": len(all_actors)
            }
        }

    except Exception as e:
        logger.error(f"construct_mansion error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_arch(
    radius: float = 300.0,
    segments: int = 6,
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "ArchBlock",
    mesh: str = "/Engine/BasicShapes/Cube.Cube"
) -> Dict[str, Any]:
    """Create a simple arch using cubes in a semicircle."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        spawned = []
        angle_step = math.pi / segments
        scale = radius / 300.0 / 2
        for i in range(segments + 1):
            theta = angle_step * i
            x = radius * math.cos(theta)
            z = radius * math.sin(theta)
            actor_name = f"{name_prefix}_{i}"
            params = {
                "name": actor_name,
                "type": "StaticMeshActor",
                "location": [location[0] + x, location[1], location[2] + z],
                "scale": [scale, scale, scale],
                "static_mesh": mesh
            }
            resp = safe_spawn_actor(unreal, params)
            if resp and resp.get("status") == "success":
                spawned.append(resp)
        return {"success": True, "actors": spawned}
    except Exception as e:
        logger.error(f"create_arch error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def spawn_physics_blueprint_actor (
    name: str,
    mesh_path: str = "/Engine/BasicShapes/Cube.Cube",
    location: List[float] = [0.0, 0.0, 0.0],
    mass: float = 1.0,
    simulate_physics: bool = True,
    gravity_enabled: bool = True,
    color: List[float] = None,  # Optional color parameter [R, G, B] or [R, G, B, A]
    scale: List[float] = [1.0, 1.0, 1.0]  # Default scale
) -> Dict[str, Any]:
    """
    Quickly spawn a single actor with physics, color, and a specific mesh.

    This is the primary function for creating simple objects with physics properties.
    It handles creating a temporary Blueprint, setting up the mesh, color, and physics,
    and then spawns the actor in the world. It's ideal for quickly adding
    dynamic objects to the scene without needing to manually create Blueprints.
    
    Args:
        color: Optional color as [R, G, B] or [R, G, B, A] where values are 0.0-1.0.
               If [R, G, B] is provided, alpha will be set to 1.0 automatically.
    """
    try:
        bp_name = f"{name}_BP"
        create_blueprint(bp_name, "Actor")
        add_component_to_blueprint(bp_name, "StaticMeshComponent", "Mesh", scale=scale)
        set_static_mesh_properties(bp_name, "Mesh", mesh_path)
        set_physics_properties(bp_name, "Mesh", simulate_physics, gravity_enabled, mass)

        # Set color if provided
        if color is not None:
            # Convert 3-value color [R,G,B] to 4-value [R,G,B,A] if needed
            if len(color) == 3:
                color = color + [1.0]  # Add alpha=1.0
            elif len(color) != 4:
                logger.warning(f"Invalid color format: {color}. Expected [R,G,B] or [R,G,B,A]. Skipping color.")
                color = None

            if color is not None:
                color_result = set_mesh_material_color(bp_name, "Mesh", color)
                if not color_result.get("success", False):
                    logger.warning(f"Failed to set color {color} for {bp_name}: {color_result.get('message', 'Unknown error')}")

        compile_blueprint(bp_name)
        result = spawn_blueprint_actor(bp_name, name, location)
        
        # Spawn the blueprint actor using helper function
        unreal = get_unreal_connection()
        result = spawn_blueprint_actor(unreal, bp_name, name, location)

        # Ensure proper scale is set on the spawned actor
        if result.get("success", False):
            spawned_name = result.get("result", {}).get("name", name)
            set_actor_transform(spawned_name, scale=scale)

        return result
    except Exception as e:
        logger.error(f"spawn_physics_blueprint_actor  error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_maze(
    rows: int = 8,
    cols: int = 8,
    cell_size: float = 300.0,
    wall_height: int = 3,
    location: List[float] = [0.0, 0.0, 0.0]
) -> Dict[str, Any]:
    """Create a proper solvable maze with entrance, exit, and guaranteed path using recursive backtracking algorithm."""
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
        import random
        spawned = []
        
        # Initialize maze grid - True means wall, False means open
        maze = [[True for _ in range(cols * 2 + 1)] for _ in range(rows * 2 + 1)]
        
        # Recursive backtracking maze generation
        def carve_path(row, col):
            # Mark current cell as path
            maze[row * 2 + 1][col * 2 + 1] = False
            
            # Random directions
            directions = [(0, 1), (1, 0), (0, -1), (-1, 0)]
            random.shuffle(directions)
            
            for dr, dc in directions:
                new_row, new_col = row + dr, col + dc
                
                # Check bounds
                if (0 <= new_row < rows and 0 <= new_col < cols and 
                    maze[new_row * 2 + 1][new_col * 2 + 1]):
                    
                    # Carve wall between current and new cell
                    maze[row * 2 + 1 + dr][col * 2 + 1 + dc] = False
                    carve_path(new_row, new_col)
        
        # Start carving from top-left corner
        carve_path(0, 0)
        
        # Create entrance and exit
        maze[1][0] = False  # Entrance on left side
        maze[rows * 2 - 1][cols * 2] = False  # Exit on right side
        
        # Build the actual maze in Unreal
        maze_height = rows * 2 + 1
        maze_width = cols * 2 + 1
        
        for r in range(maze_height):
            for c in range(maze_width):
                if maze[r][c]:  # If this is a wall
                    # Stack blocks to create wall height
                    for h in range(wall_height):
                        x_pos = location[0] + (c - maze_width/2) * cell_size
                        y_pos = location[1] + (r - maze_height/2) * cell_size
                        z_pos = location[2] + h * cell_size
                        
                        actor_name = f"Maze_Wall_{r}_{c}_{h}"
                        params = {
                            "name": actor_name,
                            "type": "StaticMeshActor",
                            "location": [x_pos, y_pos, z_pos],
                            "scale": [cell_size/100.0, cell_size/100.0, cell_size/100.0],
                            "static_mesh": "/Engine/BasicShapes/Cube.Cube"
                        }
                        resp = safe_spawn_actor(unreal, params)
                        if resp and resp.get("status") == "success":
                            spawned.append(resp)
        
        # Add entrance and exit markers
        entrance_marker = safe_spawn_actor(unreal, {
            "name": "Maze_Entrance",
            "type": "StaticMeshActor",
            "location": [location[0] - maze_width/2 * cell_size - cell_size, 
                       location[1] + (-maze_height/2 + 1) * cell_size, 
                       location[2] + cell_size],
            "scale": [0.5, 0.5, 0.5],
            "static_mesh": "/Engine/BasicShapes/Cylinder.Cylinder"
        })
        if entrance_marker and entrance_marker.get("status") == "success":
            spawned.append(entrance_marker)
            
        exit_marker = safe_spawn_actor(unreal, {
            "name": "Maze_Exit",
            "type": "StaticMeshActor", 
            "location": [location[0] + maze_width/2 * cell_size + cell_size,
                       location[1] + (-maze_height/2 + rows * 2 - 1) * cell_size,
                       location[2] + cell_size],
            "scale": [0.5, 0.5, 0.5],
            "static_mesh": "/Engine/BasicShapes/Sphere.Sphere"
        })
        if exit_marker and exit_marker.get("status") == "success":
            spawned.append(exit_marker)
        
        return {
            "success": True, 
            "actors": spawned, 
            "maze_size": f"{rows}x{cols}",
            "wall_count": len([block for block in spawned if "Wall" in block.get("name", "")]),
            "entrance": "Left side (cylinder marker)",
            "exit": "Right side (sphere marker)"
        }
    except Exception as e:
        logger.error(f"create_maze error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def get_available_materials(
    search_path: str = "/Game/",
    include_engine_materials: bool = True
) -> Dict[str, Any]:
    """Get a list of available materials in the project that can be applied to objects."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "search_path": search_path,
            "include_engine_materials": include_engine_materials
        }
        response = unreal.send_command("get_available_materials", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"get_available_materials error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def apply_material_to_actor(
    actor_name: str,
    material_path: str,
    material_slot: int = 0
) -> Dict[str, Any]:
    """Apply a specific material to an actor in the level."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "actor_name": actor_name,
            "material_path": material_path,
            "material_slot": material_slot
        }
        response = unreal.send_command("apply_material_to_actor", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"apply_material_to_actor error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def apply_material_to_blueprint(
    blueprint_name: str,
    component_name: str,
    material_path: str,
    material_slot: int = 0
) -> Dict[str, Any]:
    """Apply a specific material to a component in a Blueprint."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "material_path": material_path,
            "material_slot": material_slot
        }
        response = unreal.send_command("apply_material_to_blueprint", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"apply_material_to_blueprint error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def get_actor_material_info(
    actor_name: str
) -> Dict[str, Any]:
    """Get information about the materials currently applied to an actor."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        params = {"actor_name": actor_name}
        response = unreal.send_command("get_actor_material_info", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"get_actor_material_info error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def set_mesh_material_color(
    blueprint_name: str,
    component_name: str,
    color: List[float],
    material_path: str = "/Engine/BasicShapes/BasicShapeMaterial",
    parameter_name: str = "BaseColor",
    material_slot: int = 0
) -> Dict[str, Any]:
    """Set material color on a mesh component using the proven color system."""
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    
    try:
        # Validate color format
        if not isinstance(color, list) or len(color) != 4:
            return {"success": False, "message": "Invalid color format. Must be a list of 4 float values [R, G, B, A]."}
        
        # Ensure all color values are floats between 0 and 1
        color = [float(min(1.0, max(0.0, val))) for val in color]
        
        # Set BaseColor parameter first
        params_base = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "color": color,
            "material_path": material_path,
            "parameter_name": "BaseColor",
            "material_slot": material_slot
        }
        response_base = unreal.send_command("set_mesh_material_color", params_base)
        
        # Set Color parameter second (for maximum compatibility)
        params_color = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "color": color,
            "material_path": material_path,
            "parameter_name": "Color",
            "material_slot": material_slot
        }
        response_color = unreal.send_command("set_mesh_material_color", params_color)
        
        # Return success if either parameter setting worked
        if (response_base and response_base.get("status") == "success") or (response_color and response_color.get("status") == "success"):
            return {
                "success": True, 
                "message": f"Color applied successfully to slot {material_slot}: {color}",
                "base_color_result": response_base,
                "color_result": response_color,
                "material_slot": material_slot
            }
        else:
            return {
                "success": False, 
                "message": f"Failed to set color parameters on slot {material_slot}. BaseColor: {response_base}, Color: {response_color}"
            }
            
    except Exception as e:
        logger.error(f"set_mesh_material_color error: {e}")
        return {"success": False, "message": str(e)}

# Advanced Town Generation System
@mcp.tool()
def create_town(
    town_size: str = "medium",  # "small", "medium", "large", "metropolis"
    building_density: float = 0.7,  # 0.0 to 1.0
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "Town",
    include_infrastructure: bool = True,
    architectural_style: str = "mixed"  # "modern", "cottage", "mansion", "mixed", "downtown", "futuristic"
) -> Dict[str, Any]:
    """Create a full dynamic town with buildings, streets, infrastructure, and vehicles."""
    try:
        import random
        random.seed()  # Use different seed each time for variety
        
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        
        logger.info(f"Creating {town_size} town with {building_density} density at {location}")
        
        # Define town parameters based on size
        town_params = {
            "small": {"blocks": 3, "block_size": 1500, "max_building_height": 5, "population": 20, "skyscraper_chance": 0.1},
            "medium": {"blocks": 5, "block_size": 2000, "max_building_height": 10, "population": 50, "skyscraper_chance": 0.3},
            "large": {"blocks": 7, "block_size": 2500, "max_building_height": 20, "population": 100, "skyscraper_chance": 0.5},
            "metropolis": {"blocks": 10, "block_size": 3000, "max_building_height": 40, "population": 200, "skyscraper_chance": 0.7}
        }
        
        params = town_params.get(town_size, town_params["medium"])
        blocks = params["blocks"]
        block_size = params["block_size"]
        max_height = params["max_building_height"]
        target_population = int(params["population"] * building_density)
        skyscraper_chance = params["skyscraper_chance"]
        
        all_spawned = []
        street_width = block_size * 0.3
        building_area = block_size * 0.7
        
        # Create street grid first
        logger.info("Creating street grid...")
        street_results = _create_street_grid(blocks, block_size, street_width, location, name_prefix)
        all_spawned.extend(street_results.get("actors", []))
        
        # Create buildings in each block
        logger.info("Placing buildings...")
        building_count = 0
        for block_x in range(blocks):
            for block_y in range(blocks):
                if building_count >= target_population:
                    break
                    
                # Skip some blocks randomly for variety
                if random.random() > building_density:
                    continue
                
                block_center_x = location[0] + (block_x - blocks/2) * block_size
                block_center_y = location[1] + (block_y - blocks/2) * block_size
                
                # Randomly choose building type based on style and location
                if architectural_style == "downtown" or architectural_style == "futuristic":
                    building_types = ["skyscraper", "office_tower", "apartment_complex", "shopping_mall", "parking_garage", "hotel"]
                elif architectural_style == "mixed":
                    # Central blocks get taller buildings
                    is_central = abs(block_x - blocks//2) <= 1 and abs(block_y - blocks//2) <= 1
                    if is_central and random.random() < skyscraper_chance:
                        building_types = ["skyscraper", "office_tower", "apartment_complex", "hotel", "shopping_mall"]
                    else:
                        building_types = ["house", "tower", "mansion", "commercial", "apartment_building", "restaurant", "store"]
                else:
                    building_types = [architectural_style] * 3 + ["commercial", "restaurant", "store"]
                
                building_type = random.choice(building_types)
                
                # Create building with variety
                building_result = _create_town_building(
                    building_type, 
                    [block_center_x, block_center_y, location[2]],
                    building_area,
                    max_height,
                    f"{name_prefix}_Building_{block_x}_{block_y}",
                    building_count
                )
                
                if building_result.get("status") == "success":
                    all_spawned.extend(building_result.get("actors", []))
                    building_count += 1
        
        # Add infrastructure if requested
        infrastructure_count = 0
        if include_infrastructure:
            logger.info("Adding infrastructure...")
            
            # Street lights
            light_results = _create_street_lights(blocks, block_size, location, name_prefix)
            all_spawned.extend(light_results.get("actors", []))
            infrastructure_count += len(light_results.get("actors", []))
            
            # Vehicles
            vehicle_results = _create_town_vehicles(blocks, block_size, street_width, location, name_prefix, target_population // 3)
            all_spawned.extend(vehicle_results.get("actors", []))
            infrastructure_count += len(vehicle_results.get("actors", []))
            
            # Parks and decorations
            decoration_results = _create_town_decorations(blocks, block_size, location, name_prefix)
            all_spawned.extend(decoration_results.get("actors", []))
            infrastructure_count += len(decoration_results.get("actors", []))
            
            
            # Add advanced infrastructure
            logger.info("Adding advanced infrastructure...")
            
            # Traffic lights at intersections
            traffic_results = _create_traffic_lights(blocks, block_size, location, name_prefix)
            all_spawned.extend(traffic_results.get("actors", []))
            infrastructure_count += len(traffic_results.get("actors", []))
            
            # Street signs and billboards
            signage_results = _create_street_signage(blocks, block_size, location, name_prefix, town_size)
            all_spawned.extend(signage_results.get("actors", []))
            infrastructure_count += len(signage_results.get("actors", []))
            
            # Sidewalks and crosswalks
            sidewalk_results = _create_sidewalks_crosswalks(blocks, block_size, street_width, location, name_prefix)
            all_spawned.extend(sidewalk_results.get("actors", []))
            infrastructure_count += len(sidewalk_results.get("actors", []))
            
            # Urban furniture (benches, trash cans, bus stops)
            furniture_results = _create_urban_furniture(blocks, block_size, location, name_prefix)
            all_spawned.extend(furniture_results.get("actors", []))
            infrastructure_count += len(furniture_results.get("actors", []))
            
            # Parking meters and hydrants
            utility_results = _create_street_utilities(blocks, block_size, location, name_prefix)
            all_spawned.extend(utility_results.get("actors", []))
            infrastructure_count += len(utility_results.get("actors", []))
            
            # Add plaza/square in center for large towns
            if town_size in ["large", "metropolis"]:
                plaza_results = _create_central_plaza(blocks, block_size, location, name_prefix)
                all_spawned.extend(plaza_results.get("actors", []))
                infrastructure_count += len(plaza_results.get("actors", []))
        
        return {
            "success": True,
            "town_stats": {
                "size": town_size,
                "density": building_density,
                "blocks": blocks,
                "buildings": building_count,
                "infrastructure_items": infrastructure_count,
                "total_actors": len(all_spawned),
                "architectural_style": architectural_style
            },
            "actors": all_spawned,
            "message": f"Created {town_size} town with {building_count} buildings and {infrastructure_count} infrastructure items"
        }
        
    except Exception as e:
        logger.error(f"create_town error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def create_castle_fortress(
    castle_size: str = "large",  # "small", "medium", "large", "epic"
    location: List[float] = [0.0, 0.0, 0.0],
    name_prefix: str = "Castle",
    include_siege_weapons: bool = True,
    include_village: bool = True,
    architectural_style: str = "medieval"  # "medieval", "fantasy", "gothic"
) -> Dict[str, Any]:
    """
    Create a massive castle fortress with walls, towers, courtyards, throne room,
    and surrounding village. Perfect for dramatic TikTok reveals showing
    the scale and detail of a complete medieval fortress.
    """
    try:
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        
        logger.info(f"Creating {castle_size} {architectural_style} castle fortress")
        all_actors = []
        
        # Get size parameters and calculate scaled dimensions
        params = get_castle_size_params(castle_size)
        dimensions = calculate_scaled_dimensions(params, scale_factor=2.0)
        
        # Build castle components using helper functions
        build_outer_bailey_walls(unreal, name_prefix, location, dimensions, all_actors)
        build_inner_bailey_walls(unreal, name_prefix, location, dimensions, all_actors)
        build_gate_complex(unreal, name_prefix, location, dimensions, all_actors)
        build_corner_towers(unreal, name_prefix, location, dimensions, architectural_style, all_actors)
        build_inner_corner_towers(unreal, name_prefix, location, dimensions, all_actors)
        build_intermediate_towers(unreal, name_prefix, location, dimensions, all_actors)
        build_central_keep(unreal, name_prefix, location, dimensions, all_actors)
        build_courtyard_complex(unreal, name_prefix, location, dimensions, all_actors)
        build_bailey_annexes(unreal, name_prefix, location, dimensions, all_actors)
        
        # Add optional components
        if include_siege_weapons:
            build_siege_weapons(unreal, name_prefix, location, dimensions, all_actors)
        
        if include_village:
            build_village_settlement(unreal, name_prefix, location, dimensions, castle_size, all_actors)
        
        # Add final touches
        build_drawbridge_and_moat(unreal, name_prefix, location, dimensions, all_actors)
        add_decorative_flags(unreal, name_prefix, location, dimensions, all_actors)
        
        logger.info(f"Castle fortress creation complete! Created {len(all_actors)} actors")

        
        return {
            "success": True,
            "message": f"Epic {castle_size} {architectural_style} castle fortress created with {len(all_actors)} elements!",
            "actors": all_actors,
            "stats": {
                "size": castle_size,
                "style": architectural_style,
                "wall_sections": int(dimensions["outer_width"]/200) * 2 + int(dimensions["outer_depth"]/200) * 2,
                "towers": dimensions["tower_count"],
                "has_village": include_village,
                "has_siege_weapons": include_siege_weapons,
                "total_actors": len(all_actors)
            }
        }
        
    except Exception as e:
        logger.error(f"create_castle_fortress error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_suspension_bridge(
    span_length: float = 6000.0,
    deck_width: float = 800.0,
    tower_height: float = 4000.0,
    cable_sag_ratio: float = 0.12,
    module_size: float = 200.0,
    location: List[float] = [0.0, 0.0, 0.0],
    orientation: str = "x",
    name_prefix: str = "Bridge",
    deck_mesh: str = "/Engine/BasicShapes/Cube.Cube",
    tower_mesh: str = "/Engine/BasicShapes/Cube.Cube",
    cable_mesh: str = "/Engine/BasicShapes/Cylinder.Cylinder",
    suspender_mesh: str = "/Engine/BasicShapes/Cylinder.Cylinder",
    dry_run: bool = False
) -> Dict[str, Any]:
    """
    Build a suspension bridge with towers, deck, cables, and suspenders.
    
    Creates a realistic suspension bridge with parabolic main cables, vertical
    suspenders, twin towers, and a multi-lane deck. Perfect for dramatic reveals
    showing engineering marvels.
    
    Args:
        span_length: Total span between towers
        deck_width: Width of the bridge deck
        tower_height: Height of support towers
        cable_sag_ratio: Sag as fraction of span (0.1-0.15 typical)
        module_size: Resolution for segments (affects actor count)
        location: Center point of the bridge
        orientation: "x" or "y" for bridge direction
        name_prefix: Prefix for all spawned actors
        deck_mesh: Mesh for deck segments
        tower_mesh: Mesh for tower components
        cable_mesh: Mesh for cable segments
        suspender_mesh: Mesh for vertical suspenders
        dry_run: If True, calculate metrics without spawning
    
    Returns:
        Dictionary with success status, spawned actors, and performance metrics
    """
    try:
        import time
        start_time = time.perf_counter()
        
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        
        logger.info(f"Creating suspension bridge: span={span_length}, width={deck_width}, height={tower_height}")
        
        all_actors = []
        
        # Calculate expected actor counts for dry run
        if dry_run:
            expected_towers = 10  # 2 towers with main, base, top, and 2 attachment points each
            expected_deck = max(1, int(span_length / module_size)) * max(1, int(deck_width / module_size))
            expected_cables = 2 * max(1, int(span_length / module_size))  # 2 main cables
            expected_suspenders = 2 * max(1, int(span_length / (module_size * 3)))  # Every 3 modules
            
            elapsed_ms = int((time.perf_counter() - start_time) * 1000)
            
            return {
                "success": True,
                "dry_run": True,
                "metrics": {
                    "total_actors": expected_towers + expected_deck + expected_cables + expected_suspenders,
                    "deck_segments": expected_deck,
                    "cable_segments": expected_cables,
                    "suspender_count": expected_suspenders,
                    "towers": expected_towers,
                    "span_length": span_length,
                    "deck_width": deck_width,
                    "est_area": span_length * deck_width,
                    "elapsed_ms": elapsed_ms
                }
            }
        
        # Build the bridge structure
        counts = build_suspension_bridge_structure(
            unreal,
            span_length,
            deck_width,
            tower_height,
            cable_sag_ratio,
            module_size,
            location,
            orientation,
            name_prefix,
            deck_mesh,
            tower_mesh,
            cable_mesh,
            suspender_mesh,
            all_actors
        )
        
        # Calculate metrics
        elapsed_ms = int((time.perf_counter() - start_time) * 1000)
        total_actors = sum(counts.values())
        
        logger.info(f"Bridge construction complete: {total_actors} actors in {elapsed_ms}ms")
        
        return {
            "success": True,
            "message": f"Created suspension bridge with {total_actors} components",
            "actors": all_actors,
            "metrics": {
                "total_actors": total_actors,
                "deck_segments": counts["deck_segments"],
                "cable_segments": counts["cable_segments"],
                "suspender_count": counts["suspenders"],
                "towers": counts["towers"],
                "span_length": span_length,
                "deck_width": deck_width,
                "est_area": span_length * deck_width,
                "elapsed_ms": elapsed_ms
            }
        }
        
    except Exception as e:
        logger.error(f"create_suspension_bridge error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_aqueduct(
    arches: int = 18,
    arch_radius: float = 600.0,
    pier_width: float = 200.0,
    tiers: int = 2,
    deck_width: float = 600.0,
    module_size: float = 200.0,
    location: List[float] = [0.0, 0.0, 0.0],
    orientation: str = "x",
    name_prefix: str = "Aqueduct",
    arch_mesh: str = "/Engine/BasicShapes/Cylinder.Cylinder",
    pier_mesh: str = "/Engine/BasicShapes/Cube.Cube",
    deck_mesh: str = "/Engine/BasicShapes/Cube.Cube",
    dry_run: bool = False
) -> Dict[str, Any]:
    """
    Build a multi-tier Roman-style aqueduct with arches and water channel.
    
    Creates a majestic aqueduct with repeating arches, support piers, and
    a water channel deck. Each tier has progressively smaller piers for
    realistic tapering. Perfect for showing ancient engineering.
    
    Args:
        arches: Number of arches per tier
        arch_radius: Radius of each arch
        pier_width: Width of support piers
        tiers: Number of vertical tiers (1-3 recommended)
        deck_width: Width of the water channel
        module_size: Resolution for segments (affects actor count)
        location: Starting point of the aqueduct
        orientation: "x" or "y" for aqueduct direction
        name_prefix: Prefix for all spawned actors
        arch_mesh: Mesh for arch segments (cylinder)
        pier_mesh: Mesh for support piers
        deck_mesh: Mesh for deck and walls
        dry_run: If True, calculate metrics without spawning
    
    Returns:
        Dictionary with success status, spawned actors, and performance metrics
    """
    try:
        import time
        start_time = time.perf_counter()
        
        unreal = get_unreal_connection()
        if not unreal:
            return {"success": False, "message": "Failed to connect to Unreal Engine"}
        
        logger.info(f"Creating aqueduct: {arches} arches, {tiers} tiers, radius={arch_radius}")
        
        all_actors = []
        
        # Calculate dimensions
        total_length = arches * (2 * arch_radius + pier_width) + pier_width
        
        # Calculate expected actor counts for dry run
        if dry_run:
            # Arch segments per arch based on semicircle circumference
            arch_circumference = math.pi * arch_radius
            segments_per_arch = max(4, int(arch_circumference / module_size))
            expected_arch_segments = tiers * arches * segments_per_arch
            
            # Piers: (arches + 1) per tier
            expected_piers = tiers * (arches + 1)
            
            # Deck segments including side walls
            deck_length_segments = max(1, int(total_length / module_size))
            deck_width_segments = max(1, int(deck_width / module_size))
            expected_deck = deck_length_segments * deck_width_segments
            expected_deck += 2 * deck_length_segments  # Side walls
            
            elapsed_ms = int((time.perf_counter() - start_time) * 1000)
            
            return {
                "success": True,
                "dry_run": True,
                "metrics": {
                    "total_actors": expected_arch_segments + expected_piers + expected_deck,
                    "arch_segments": expected_arch_segments,
                    "pier_count": expected_piers,
                    "tiers": tiers,
                    "deck_segments": expected_deck,
                    "total_length": total_length,
                    "est_area": total_length * deck_width,
                    "elapsed_ms": elapsed_ms
                }
            }
        
        # Build the aqueduct structure
        counts = build_aqueduct_structure(
            unreal,
            arches,
            arch_radius,
            pier_width,
            tiers,
            deck_width,
            module_size,
            location,
            orientation,
            name_prefix,
            arch_mesh,
            pier_mesh,
            deck_mesh,
            all_actors
        )
        
        # Calculate metrics
        elapsed_ms = int((time.perf_counter() - start_time) * 1000)
        total_actors = sum(counts.values())
        
        logger.info(f"Aqueduct construction complete: {total_actors} actors in {elapsed_ms}ms")
        
        return {
            "success": True,
            "message": f"Created {tiers}-tier aqueduct with {arches} arches ({total_actors} components)",
            "actors": all_actors,
            "metrics": {
                "total_actors": total_actors,
                "arch_segments": counts["arch_segments"],
                "pier_count": counts["piers"],
                "tiers": tiers,
                "deck_segments": counts["deck_segments"],
                "total_length": total_length,
                "est_area": total_length * deck_width,
                "elapsed_ms": elapsed_ms
            }
        }
        
    except Exception as e:
        logger.error(f"create_aqueduct error: {e}")
        return {"success": False, "message": str(e)}



# ============================================================================
# Blueprint Node Graph Tool
# ============================================================================

@mcp.tool()
def add_node(
    blueprint_name: str,
    node_type: str,
    pos_x: float = 0,
    pos_y: float = 0,
    message: str = "",
    event_type: str = "BeginPlay",
    variable_name: str = "",
    target_function: str = "",
    target_blueprint: Optional[str] = None,
    function_name: Optional[str] = None
) -> Dict[str, Any]:
    """
    Add a node to a Blueprint graph.

    Create various types of K2Nodes in a Blueprint's event graph or function graph.
    Supports 23 node types organized by category.

    Args:
        blueprint_name: Name of the Blueprint to modify
        node_type: Type of node to create. Supported types (23 total):

            CONTROL FLOW:
                "Branch" - Conditional execution (if/then/else)
                "Comparison" - Arithmetic/logical operators (==, !=, <, >, AND, OR, etc.)
                    ℹ️ Types can be changed via set_node_property with action="set_pin_type"
                "Switch" - Switch on byte/enum value with cases
                    ℹ️ Creates 1 pin at creation; add more via set_node_property with action="add_pin"
                "SwitchEnum" - Switch on enum type (auto-generates pins per enum value)
                    ℹ️ Creates pins based on enum; change enum via set_node_property with action="set_enum_type"
                "SwitchInteger" - Switch on integer value with cases
                    ℹ️ Creates 1 pin at creation; add more via set_node_property with action="add_pin"
                "ExecutionSequence" - Sequential execution with multiple outputs
                    ℹ️ Creates 1 pin at creation; add/remove via set_node_property (add_pin/remove_pin)

            DATA:
                "VariableGet" - Read a variable value (⚠️ variable must exist in Blueprint)
                "VariableSet" - Set a variable value (⚠️ variable must exist and be assignable)
                "MakeArray" - Create array from individual inputs
                    ℹ️ Creates 1 pin at creation; add/remove via set_node_property with action="set_num_elements"

            CASTING:
                "DynamicCast" - Cast object to specific class (⚠️ handle "Cast Failed" output)
                "ClassDynamicCast" - Cast class reference to derived class (⚠️ handle failure cases)
                "CastByteToEnum" - Convert byte value to enum (⚠️ byte must be valid enum range)

            UTILITY:
                "Print" - Debug output to screen/log (configurable duration and color)
                "CallFunction" - Call any blueprint/engine function (⚠️ function must exist)
                "Select" - Choose between two inputs based on boolean condition
                "SpawnActor" - Spawn actor from class (⚠️ class must derive from Actor)

            SPECIALIZED:
                "Timeline" - Animation timeline playback with curve tracks
                    ⚠️ REQUIRES MANUAL IMPLEMENTATION: Animation curves must be added in editor
                "GetDataTableRow" - Query row from data table (⚠️ DataTable must exist)
                "AddComponentByClass" - Dynamically add component to actor
                "Self" - Reference to current actor/object
                "Knot" - Invisible reroute node (wire organization only)

            EVENT:
                "Event" - Blueprint event (specify event_type: BeginPlay, Tick, etc.)
                    ℹ️ Tick events run every frame - be mindful of performance impact

        pos_x: X position in graph (default: 0)
        pos_y: Y position in graph (default: 0)
        message: For Print nodes, the text to print
        event_type: For Event nodes, the event name (BeginPlay, Tick, Destroyed, etc.)
        variable_name: For Variable nodes, the variable name
        target_function: For CallFunction nodes, the function to call
        target_blueprint: For CallFunction nodes, optional path to target Blueprint
        function_name: Optional name of function graph to add node to (if None, uses EventGraph)

    Returns:
        Dictionary with success status, node_id, and position

    Important Notes:
        - Most nodes can have pins modified after creation via set_node_property
        - Dynamic pin management: Switch/SwitchEnum/ExecutionSequence/MakeArray support pin operations
        - Timeline is the ONLY node requiring manual implementation (curves must be added in editor)
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        node_params = {
            "pos_x": pos_x,
            "pos_y": pos_y
        }

        if message:
            node_params["message"] = message
        if event_type:
            node_params["event_type"] = event_type
        if variable_name:
            node_params["variable_name"] = variable_name
        if target_function:
            node_params["target_function"] = target_function
        if target_blueprint:
            node_params["target_blueprint"] = target_blueprint
        if function_name:
            node_params["function_name"] = function_name

        result = node_manager.add_node(
            unreal,
            blueprint_name,
            node_type,
            node_params
        )

        return result

    except Exception as e:
        logger.error(f"add_node error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def connect_nodes(
    blueprint_name: str,
    source_node_id: str,
    source_pin_name: str,
    target_node_id: str,
    target_pin_name: str,
    function_name: Optional[str] = None
) -> Dict[str, Any]:
    """
    Connect two nodes in a Blueprint graph.

    Links a source pin to a target pin between existing nodes in a Blueprint's event graph or function graph.

    Args:
        blueprint_name: Name of the Blueprint to modify
        source_node_id: ID of the source node
        source_pin_name: Name of the output pin on the source node
        target_node_id: ID of the target node
        target_pin_name: Name of the input pin on the target node
        function_name: Optional name of function graph (if None, uses EventGraph)

    Returns:
        Dictionary with success status and connection details
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = connector_manager.connect_nodes(
            unreal,
            blueprint_name,
            source_node_id,
            source_pin_name,
            target_node_id,
            target_pin_name,
            function_name
        )

        return result
    except Exception as e:
        logger.error(f"connect_nodes error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def create_variable(
    blueprint_name: str,
    variable_name: str,
    variable_type: str,
    default_value: Any = None,
    is_public: bool = False,
    tooltip: str = "",
    category: str = "Default"
) -> Dict[str, Any]:
    """
    Create a variable in a Blueprint.

    Adds a new variable to a Blueprint with specified type, default value, and properties.

    Args:
        blueprint_name: Name of the Blueprint to modify
        variable_name: Name of the variable to create
        variable_type: Type of the variable ("bool", "int", "float", "string", "vector", "rotator")
        default_value: Default value for the variable (optional)
        is_public: Whether the variable should be public/editable (default: False)
        tooltip: Tooltip text for the variable (optional)
        category: Category for organizing variables (default: "Default")

    Returns:
        Dictionary with success status and variable details
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = variable_manager.create_variable(
            unreal,
            blueprint_name,
            variable_name,
            variable_type,
            default_value,
            is_public,
            tooltip,
            category
        )

        return result
    except Exception as e:
        logger.error(f"create_variable error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def set_blueprint_variable_properties(
    blueprint_name: str,
    variable_name: str,
    var_name: Optional[str] = None,
    var_type: Optional[str] = None,
    is_blueprint_readable: Optional[bool] = None,
    is_blueprint_writable: Optional[bool] = None,
    is_public: Optional[bool] = None,
    is_editable_in_instance: Optional[bool] = None,
    tooltip: Optional[str] = None,
    category: Optional[str] = None,
    default_value: Any = None,
    expose_on_spawn: Optional[bool] = None,
    expose_to_cinematics: Optional[bool] = None,
    slider_range_min: Optional[str] = None,
    slider_range_max: Optional[str] = None,
    value_range_min: Optional[str] = None,
    value_range_max: Optional[str] = None,
    units: Optional[str] = None,
    bitmask: Optional[bool] = None,
    bitmask_enum: Optional[str] = None,
    replication_enabled: Optional[bool] = None,
    replication_condition: Optional[int] = None,
    is_private: Optional[bool] = None
) -> Dict[str, Any]:
    """
    Modify properties of an existing Blueprint variable without deleting it.

    Preserves all VariableGet and VariableSet nodes connected to this variable.

    Args:
        blueprint_name: Name of the Blueprint to modify
        variable_name: Name of the variable to modify

        var_name: Rename the variable (optional)
            ✅ PASS - VarDesc->VarName works correctly

        var_type: Change variable type (optional)
            ✅ PASS - VarDesc->VarType works correctly (int→float returns "real")

        is_blueprint_readable: Allow reading in Blueprint (VariableGet) (optional)
            ✅ PASS - CPF_BlueprintReadOnly flag (inverted logic)

        is_blueprint_writable: Allow writing in Blueprint (Set) (optional)
            ✅ PASS - CPF_BlueprintReadOnly flag (inverted logic)
            ⚠️ NOT returned by get_variable_details()

        is_public: Visible in Blueprint editor (optional)
            ✅ PASS - Controls variable visibility

        is_editable_in_instance: Modifiable on instances (optional)
            ✅ PASS - CPF_DisableEditOnInstance flag (inverted logic)

        tooltip: Variable description (optional)
            ✅ PASS - Metadata MD_Tooltip works correctly

        category: Variable category (optional)
            ✅ PASS - Direct property Category works

        default_value: New default value (optional)
            ✅ PASS - Works but get_variable_details() returns empty string

        expose_on_spawn: Show in spawn dialog (optional)
            ✅ PASS - Metadata MD_ExposeOnSpawn works
            ⚠️ Requires is_editable_in_instance=true to be visible
            ⚠️ NOT returned by get_variable_details()

        expose_to_cinematics: Expose to cinematics (optional)
            ✅ PASS - CPF_Interp flag works correctly
            ⚠️ NOT returned by get_variable_details()

        slider_range_min: UI slider minimum value (optional)
            ✅ PASS - Metadata MD_UIMin works (string value)
            ⚠️ NOT returned by get_variable_details()

        slider_range_max: UI slider maximum value (optional)
            ✅ PASS - Metadata MD_UIMax works (string value)
            ⚠️ NOT returned by get_variable_details()

        value_range_min: Clamp minimum value (optional)
            ✅ PASS - Metadata MD_ClampMin works (string value)
            ⚠️ NOT returned by get_variable_details()

        value_range_max: Clamp maximum value (optional)
            ✅ PASS - Metadata MD_ClampMax works (string value)
            ⚠️ NOT returned by get_variable_details()

        units: Display units (optional)
            ⚠️ PARTIAL - Metadata MD_Units works for value display (e.g., "0.0 cm")
            ❌ UI dropdown stays at "None" (Unreal Editor limitation - dropdown doesn't sync with metadata)
            ⚠️ Use long format: "Centimeters", "Meters" (not "cm", "m")
            ⚠️ NOT returned by get_variable_details()

        bitmask: Treat as bitmask (optional)
            ✅ PASS - Metadata TEXT("Bitmask") works correctly
            ⚠️ NOT returned by get_variable_details()

        bitmask_enum: Bitmask enum type (optional)
            ✅ PASS - Metadata TEXT("BitmaskEnum") works
            ⚠️ REQUIRES full path format: "/Script/ModuleName.EnumName"
            ❌ Short names generate warning and don't sync dropdown
            ✅ Validated enums (use FULL PATHS):
                - /Script/UniversalObjectLocator.ELocatorResolveFlags
                - /Script/JsonObjectGraph.EJsonStringifyFlags
                - /Script/MediaAssets.EMediaAudioCaptureDeviceFilter
                - /Script/MediaAssets.EMediaVideoCaptureDeviceFilter
                - /Script/MediaAssets.EMediaWebcamCaptureDeviceFilter
                - /Script/Engine.EAnimAssetCurveFlags
                - /Script/Engine.EHardwareDeviceSupportedFeatures
                - /Script/EnhancedInput.EMappingQueryIssue
                - /Script/EnhancedInput.ETriggerEvent
            ⚠️ NOT returned by get_variable_details()

        replication_enabled: Enable network replication (CPF_Net flag) (optional)
            ✅ PASS - CPF_Net flag works - Changes "Replication" dropdown (None ↔ Replicated)
            ⚠️ NOT returned by get_variable_details()

        replication_condition: Network replication condition (ELifetimeCondition 0-7) (optional)
            ✅ PASS - VarDesc->ReplicationCondition works
            ✅ Changes "Replication Condition" dropdown (e.g., None → Initial Only)
            ⚠️ Values: 0=None, 1=InitialOnly, 2=OwnerOnly, 3=SkipOwner, 4=SimulatedOnly, 5=AutonomousOnly, 6=SimulatedOrPhysics, 7=InitialOrOwner
            ✅ Returned by get_variable_details() as "replication"

        is_private: Set variable as private (optional)
            ❌ UNRESOLVED - Property flag/metadata not yet identified
            ⚠️ Attempted CPF_NativeAccessSpecifierPrivate flag and MD_AllowPrivateAccess metadata - neither work
            ⚠️ The property that controls "Privé" (Private) checkbox remains unknown
            ⚠️ Parameter exists but has no effect on UI - do NOT use until resolved

    Returns:
        Dictionary with success status and updated properties
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = variable_manager.set_blueprint_variable_properties(
            unreal,
            blueprint_name,
            variable_name,
            var_name,
            var_type,
            is_blueprint_readable,
            is_blueprint_writable,
            is_public,
            is_editable_in_instance,
            tooltip,
            category,
            default_value,
            expose_on_spawn,
            expose_to_cinematics,
            slider_range_min,
            slider_range_max,
            value_range_min,
            value_range_max,
            units,
            bitmask,
            bitmask_enum,
            replication_enabled,
            replication_condition,
            is_private
        )

        return result
    except Exception as e:
        logger.error(f"set_blueprint_variable_properties error: {e}")
        return {"success": False, "message": str(e)}

@mcp.tool()
def add_event_node(
    blueprint_name: str,
    event_name: str,
    pos_x: float = 0,
    pos_y: float = 0
) -> Dict[str, Any]:
    """
    Add an event node to a Blueprint graph.

    Create specialized event nodes (ReceiveBeginPlay, ReceiveTick, etc.)
    in a Blueprint's event graph at specified positions.

    Args:
        blueprint_name: Name of the Blueprint to modify
        event_name: Name of the event (e.g., "ReceiveBeginPlay", "ReceiveTick", "ReceiveDestroyed")
        pos_x: X position in graph (default: 0)
        pos_y: Y position in graph (default: 0)

    Returns:
        Dictionary with success status, node_id, event_name, and position
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = event_manager.add_event_node(
            unreal,
            blueprint_name,
            event_name,
            pos_x,
            pos_y
        )

        return result
    except Exception as e:
        logger.error(f"add_event_node error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def delete_node(
    blueprint_name: str,
    node_id: str,
    function_name: Optional[str] = None
) -> Dict[str, Any]:
    """
    Delete a node from a Blueprint graph.

    Removes a node and all its connections from either the EventGraph
    or a specific function graph.

    Args:
        blueprint_name: Name of the Blueprint to modify
        node_id: ID of the node to delete (NodeGuid or node name)
        function_name: Name of function graph (optional, defaults to EventGraph)

    Returns:
        Dictionary with success status and deleted_node_id
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = node_deleter.delete_node(
            unreal,
            blueprint_name,
            node_id,
            function_name
        )
        return result
    except Exception as e:
        logger.error(f"delete_node error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def set_node_property(
    blueprint_name: str,
    node_id: str,
    property_name: str = "",
    property_value: Any = None,
    function_name: Optional[str] = None,
    action: Optional[str] = None,
    pin_type: Optional[str] = None,
    pin_name: Optional[str] = None,
    enum_type: Optional[str] = None,
    new_type: Optional[str] = None,
    target_type: Optional[str] = None,
    target_function: Optional[str] = None,
    target_class: Optional[str] = None,
    event_type: Optional[str] = None
) -> Dict[str, Any]:
    """
    Set a property on a Blueprint node or perform semantic node editing.

    This function supports both simple property modifications and advanced semantic
    node editing operations (pin management, type modifications, reference updates).

    Args:
        blueprint_name: Name of the Blueprint to modify
        node_id: ID of the node to modify
        property_name: Name of property to set (legacy mode, used if action not specified)
        property_value: Value to set (legacy mode)
        function_name: Name of function graph (optional, defaults to EventGraph)
        action: Semantic action to perform - can be one of:
            Phase 1 (Pin Management):
                - "add_pin": Add a pin to a node (requires pin_type)
                - "remove_pin": Remove a pin from a node (requires pin_name)
                - "set_enum_type": Set enum type on a node (requires enum_type)
            Phase 2 (Type Modification):
                - "set_pin_type": Change pin type on comparison nodes (requires pin_name, new_type)
                - "set_value_type": Change value type on select nodes (requires new_type)
                - "set_cast_target": Change cast target type (requires target_type)
            Phase 3 (Reference Updates - DESTRUCTIVE):
                - "set_function_call": Change function being called (requires target_function)
                - "set_event_type": Change event type (requires event_type)

    Semantic action parameters:
        pin_type: Type of pin to add ("SwitchCase", "ExecutionOutput", "ArrayElement", "EnumValue")
        pin_name: Name of pin to remove or modify
        enum_type: Full path to enum type (e.g., "/Game/Enums/ECardinalDirection")
        new_type: New type for pin or value ("int", "float", "string", "bool", "vector", etc.)
        target_type: Target class path for casting
        target_function: Name of function to call
        target_class: Optional class containing the function
        event_type: Event type (e.g., "BeginPlay", "Tick", "Destroyed")

    Returns:
        Dictionary with success status and details

    Supported legacy properties by node type:
        - Print nodes: "message", "duration", "text_color"
        - Variable nodes: "variable_name"
        - All nodes: "pos_x", "pos_y", "comment"

    Examples:
        Legacy mode (set simple property):
            set_node_property(
                blueprint_name="MyActorBlueprint",
                node_id="K2Node_1234567890",
                property_name="message",
                property_value="Hello World!"
            )

        Semantic mode (add pin):
            set_node_property(
                blueprint_name="MyActorBlueprint",
                node_id="K2Node_Switch_123",
                action="add_pin",
                pin_type="SwitchCase"
            )

        Semantic mode (set enum type):
            set_node_property(
                blueprint_name="MyActorBlueprint",
                node_id="K2Node_SwitchEnum_456",
                action="set_enum_type",
                enum_type="ECardinalDirection"
            )

        Semantic mode (change function call):
            set_node_property(
                blueprint_name="MyActorBlueprint",
                node_id="K2Node_CallFunction_789",
                action="set_function_call",
                target_function="BeginPlay",
                target_class="APawn"
            )
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        # Build kwargs for semantic actions
        kwargs = {}
        if action is not None:
            if pin_type is not None:
                kwargs["pin_type"] = pin_type
            if pin_name is not None:
                kwargs["pin_name"] = pin_name
            if enum_type is not None:
                kwargs["enum_type"] = enum_type
            if new_type is not None:
                kwargs["new_type"] = new_type
            if target_type is not None:
                kwargs["target_type"] = target_type
            if target_function is not None:
                kwargs["target_function"] = target_function
            if target_class is not None:
                kwargs["target_class"] = target_class
            if event_type is not None:
                kwargs["event_type"] = event_type

        result = node_properties.set_node_property(
            unreal,
            blueprint_name,
            node_id,
            property_name,
            property_value,
            function_name,
            action,
            **kwargs
        )
        return result
    except Exception as e:
        logger.error(f"set_node_property error: {e}", exc_info=True)
        return {"success": False, "message": str(e)}


@mcp.tool()
def create_function(
    blueprint_name: str,
    function_name: str,
    return_type: str = "void"
) -> Dict[str, Any]:
    """
    Create a new function in a Blueprint.

    Args:
        blueprint_name: Name of the Blueprint to modify
        function_name: Name for the new function
        return_type: Return type of the function (default: "void")

    Returns:
        Dictionary with function_name, graph_id or error
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = function_manager.create_function_handler(
            unreal,
            blueprint_name,
            function_name,
            return_type
        )
        return result
    except Exception as e:
        logger.error(f"create_function error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def add_function_input(
    blueprint_name: str,
    function_name: str,
    param_name: str,
    param_type: str,
    is_array: bool = False
) -> Dict[str, Any]:
    """
    Add an input parameter to a Blueprint function.

    Args:
        blueprint_name: Name of the Blueprint to modify
        function_name: Name of the function
        param_name: Name of the input parameter
        param_type: Type of the parameter (bool, int, float, string, vector, etc.)
        is_array: Whether the parameter is an array (default: False)

    Returns:
        Dictionary with param_name, param_type, and direction or error
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = function_io.add_function_input_handler(
            unreal,
            blueprint_name,
            function_name,
            param_name,
            param_type,
            is_array
        )
        return result
    except Exception as e:
        logger.error(f"add_function_input error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def add_function_output(
    blueprint_name: str,
    function_name: str,
    param_name: str,
    param_type: str,
    is_array: bool = False
) -> Dict[str, Any]:
    """
    Add an output parameter to a Blueprint function.

    Args:
        blueprint_name: Name of the Blueprint to modify
        function_name: Name of the function
        param_name: Name of the output parameter
        param_type: Type of the parameter (bool, int, float, string, vector, etc.)
        is_array: Whether the parameter is an array (default: False)

    Returns:
        Dictionary with param_name, param_type, and direction or error
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = function_io.add_function_output_handler(
            unreal,
            blueprint_name,
            function_name,
            param_name,
            param_type,
            is_array
        )
        return result
    except Exception as e:
        logger.error(f"add_function_output error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def delete_function(
    blueprint_name: str,
    function_name: str
) -> Dict[str, Any]:
    """
    Delete a function from a Blueprint.

    Args:
        blueprint_name: Name of the Blueprint to modify
        function_name: Name of the function to delete

    Returns:
        Dictionary with deleted_function_name or error
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = function_manager.delete_function_handler(
            unreal,
            blueprint_name,
            function_name
        )
        return result
    except Exception as e:
        logger.error(f"delete_function error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def rename_function(
    blueprint_name: str,
    old_function_name: str,
    new_function_name: str
) -> Dict[str, Any]:
    """
    Rename a function in a Blueprint.

    Args:
        blueprint_name: Name of the Blueprint to modify
        old_function_name: Current name of the function
        new_function_name: New name for the function

    Returns:
        Dictionary with new_function_name or error
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        result = function_manager.rename_function_handler(
            unreal,
            blueprint_name,
            old_function_name,
            new_function_name
        )
        return result
    except Exception as e:
        logger.error(f"rename_function error: {e}")
        return {"success": False, "message": str(e)}


# ============================================================================
# Sproft fork additions: lower-level primitives mirroring the hosted Flop tools.
# These are clean-room implementations derived from the public UE5 API and the
# documented behaviour of the hosted tools. They are NOT derived from the
# proprietary FlopAI plugin.
# ============================================================================

@mcp.tool()
def editor_actions(
    action: str,
    asset_path: Optional[str] = None,
    only_if_dirty: bool = True,
    save_maps: bool = True,
    save_content: bool = True,
    start_location: Optional[List[float]] = None,
    start_rotation: Optional[List[float]] = None,
) -> Dict[str, Any]:
    """
    Run an editor lifecycle action: save, undo/redo, focus selection, or play.

    Mirrors the hosted Flop "editor_actions" tool. The "action" parameter
    selects which verb to perform.

    Args:
        action: One of:
            - "save_all": Save all dirty packages.
            - "save_current_level": Save the active level.
            - "save_asset": Save a specific asset (requires asset_path).
            - "undo": Undo the last editor transaction.
            - "redo": Redo the next editor transaction.
            - "focus_selection": Frame the active viewport on the selection.
            - "play": Start a Play in Editor session.
            - "stop_play": End the active PIE session.
        asset_path: Required for "save_asset". Object path like "/Game/Foo".
        only_if_dirty: When saving, only save if the asset is marked dirty.
        save_maps: For "save_all", include map packages.
        save_content: For "save_all", include content packages.
        start_location: Optional [x, y, z] PIE start location.
        start_rotation: Optional [pitch, yaw, roll] PIE start rotation.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"action": action}
    if asset_path is not None:
        params["asset_path"] = asset_path
    params["only_if_dirty"] = only_if_dirty
    params["save_maps"] = save_maps
    params["save_content"] = save_content
    if start_location is not None:
        params["start_location"] = start_location
    if start_rotation is not None:
        params["start_rotation"] = start_rotation

    try:
        response = unreal.send_command("editor_actions", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"editor_actions error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def window_capture(file_path: Optional[str] = None) -> Dict[str, Any]:
    """
    Capture a PNG screenshot of the active editor viewport.

    Mirrors the hosted Flop "window_capture" tool. The screenshot is taken
    synchronously from the active viewport and written to disk in PNG.

    Args:
        file_path: Absolute or project-relative output path. If omitted, the
            file is written to <Project>/Saved/MCPScreenshots/Capture_<ts>.png.
            A ".png" extension will be appended if missing.

    Returns:
        Dictionary with file_path, width, height, and byte_size on success.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if file_path is not None:
        params["file_path"] = file_path

    try:
        response = unreal.send_command("window_capture", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"window_capture error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def asset_factory(
    asset_type: str,
    package_path: str,
    row_struct: Optional[str] = None,
    entries: Optional[List[str]] = None,
    fields: Optional[List[Dict[str, str]]] = None,
    data_asset_class: Optional[str] = None,
    properties: Optional[Dict[str, Any]] = None,
    save: bool = True,
    overwrite: bool = False,
    actions: Optional[List[Dict[str, Any]]] = None,
    mappings: Optional[List[Dict[str, Any]]] = None,
    imc_name: Optional[str] = None,
    action_prefix: Optional[str] = None,
) -> Dict[str, Any]:
    """
    Create a new asset of the requested type.

    Mirrors the hosted Flop "asset_factory" surface. This fork supports five
    asset types so far: DataTable, Enum, Struct, DataAsset, and Enhanced
    Input bundles (one IMC plus N input actions in a single declarative
    call).

    Args:
        asset_type: One of "datatable", "enum", "struct", "data_asset",
            "enhanced_input_bundle".
        package_path: Absolute content-browser path for the new asset.
            For "enhanced_input_bundle" this is treated as the package
            root (e.g. "/Game/Input"); the IMC goes at
            "<root>/<imc_name>" and each action goes under
            "<root>/Actions/<action_prefix><name>". For the other types
            it is the asset's own path (e.g. "/Game/Data/MyTable"). A
            trailing ".AssetName" suffix is allowed and stripped.
        row_struct: For DataTable: a UScriptStruct path or short name to use
            as the row schema. Pass either a full path like
            "/Script/MyModule.MyRow", a Blueprint struct path like
            "/Game/Data/MyRow.MyRow_C", or a known engine struct short name.
        entries: For Enum: a list of entry display names, e.g.
            ["Wood", "Stone", "Berry"].
        fields: For Struct: a list of {"name", "type"} objects describing
            each member. Supported type strings: bool, int, int64, float,
            string, name, text, vector, rotator, transform, color, or a
            "/Game/..."-rooted path to an existing UScriptStruct.
        data_asset_class: For DataAsset: the UDataAsset subclass to
            instantiate. Pass a /Script-rooted class path
            ("/Script/Engine.PrimaryDataAsset"), a /Game-rooted Blueprint
            class path ("/Game/Data/MyDA.MyDA_C"), or a short engine class
            name. Defaults to UDataAsset.
        properties: For DataAsset: a flat dict of UPROPERTY name -> value to
            apply through reflection. Strings, numbers, and booleans are
            passed verbatim to FProperty::ImportText; nested objects /
            arrays are JSON-encoded first. Unknown or malformed entries are
            reported in the "skipped" field of the response without
            aborting the create.
        actions: For "enhanced_input_bundle": list of action specs.
            Each spec is a dict with keys "name" (required, e.g. "Move"),
            optional "value_type" (one of "bool" / "axis1d" / "axis2d" /
            "axis3d", default "bool"), optional "trigger_when_paused"
            (bool), and optional "description". Existing assets at the
            target path are reused unless overwrite=True.
        mappings: For "enhanced_input_bundle": list of binding rows.
            Each row is a dict with keys "action" (must match a name in
            ``actions``), "key" (FKey FName like "SpaceBar", "W",
            "Gamepad_FaceButton_Bottom"), optional "negate" (bool, adds
            a UInputModifierNegate), and optional "swizzle" (one of
            YXZ / ZYX / XZY / YZX / ZXY, adds a
            UInputModifierSwizzleAxis).
        imc_name: For "enhanced_input_bundle": IMC asset short name.
            Defaults to "IMC_Default".
        action_prefix: For "enhanced_input_bundle": prefix prepended to
            each action name. Defaults to "IA_". Names that already
            start with the prefix are not double-prefixed.
        save: Save the asset to disk after creating it. Defaults to True.
        overwrite: If an asset already exists at package_path, overwrite it.
            Defaults to False (the call fails instead).

    Returns:
        Dictionary with asset metadata on success.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "asset_type": asset_type,
        "package_path": package_path,
        "save": save,
        "overwrite": overwrite,
    }
    if row_struct is not None:
        params["row_struct"] = row_struct
    if entries is not None:
        params["entries"] = entries
    if fields is not None:
        params["fields"] = fields
    if data_asset_class is not None:
        params["data_asset_class"] = data_asset_class
    if properties is not None:
        params["properties"] = properties
    if actions is not None:
        params["actions"] = actions
    if mappings is not None:
        params["mappings"] = mappings
    if imc_name is not None:
        params["imc_name"] = imc_name
    if action_prefix is not None:
        params["action_prefix"] = action_prefix

    try:
        response = unreal.send_command("asset_factory", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"asset_factory error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def widget_edit(
    operation: str,
    package_path: Optional[str] = None,
    widget_blueprint: Optional[str] = None,
    parent_class: Optional[str] = None,
    root_panel_class: Optional[str] = None,
    widget_type: Optional[str] = None,
    widget_name: Optional[str] = None,
    parent_name: Optional[str] = None,
    text: Optional[str] = None,
    properties: Optional[Dict[str, Any]] = None,
    expose_as_variable: bool = True,
    save: bool = True,
    overwrite: bool = False,
) -> Dict[str, Any]:
    """
    Edit a Widget Blueprint asset.

    Mirrors a small slice of the hosted Flop "widget_edit" tool. Three
    operations are supported:
        - "create_widget_blueprint": create a UWidgetBlueprint at a path with
          a parent UUserWidget class and an optional root panel class.
        - "add_child_widget": construct a named widget (e.g. vertical_box,
          progress_bar, text_block, button, image) and attach it to an
          existing parent panel inside an existing Widget Blueprint.
        - "set_slot_property": apply a flat property dict to the UPanelSlot
          of an existing widget. Covers UCanvasPanelSlot anchors / offsets /
          size, UVerticalBoxSlot / UHorizontalBoxSlot padding / fill, and
          any other UPanelSlot-derived class without us spelling out each
          property by name. Properties go through
          ``FProperty::ImportText_InContainer``.

    Args:
        operation: "create_widget_blueprint", "add_child_widget", or
            "set_slot_property".
        package_path: For create: the absolute content-browser path for the
            new asset, e.g. "/Game/UI/WBP_CraftingMenu".
        widget_blueprint: For add_child_widget / set_slot_property: the
            absolute path to the existing widget blueprint, e.g.
            "/Game/UI/WBP_CraftingMenu".
        parent_class: For create: a UUserWidget subclass path or short name.
            Defaults to UUserWidget.
        root_panel_class: For create: a UPanelWidget subclass path or short
            name to seed the root widget. Defaults to UCanvasPanel.
        widget_type: For add_child_widget: the widget type to construct. One
            of vertical_box, horizontal_box, canvas_panel, overlay, scroll_box,
            border, size_box, spacer, progress_bar, text_block, button, image,
            or a fully qualified UWidget class path.
        widget_name: For add_child_widget / set_slot_property: the FName of
            the target widget. For add_child_widget the name must be unique
            inside the asset. For set_slot_property it must resolve through
            ``UWidgetTree::FindWidget``.
        parent_name: For add_child_widget: the FName of the parent panel
            inside the asset. If omitted, the asset's root panel is used.
        text: For add_child_widget: optional initial text for text_block.
        properties: For set_slot_property: the flat property dict applied to
            the widget's Slot. Examples:
                UCanvasPanelSlot:
                    {"Anchors": "(Minimum=(X=0.5,Y=0.5),Maximum=(X=0.5,Y=0.5))",
                     "Offsets": "(Left=-100,Top=-50,Right=200,Bottom=100)",
                     "Alignment": "(X=0.5,Y=0.5)",
                     "ZOrder": 1}
                UVerticalBoxSlot:
                    {"Padding": "(Left=4,Top=4,Right=4,Bottom=4)",
                     "Size": "(SizeRule=Fill,Value=1.0)",
                     "HorizontalAlignment": "HAlign_Fill",
                     "VerticalAlignment": "VAlign_Top"}
            Each entry that fails to resolve as a UPROPERTY or refuses
            ``ImportText`` is reported under the response's ``skipped`` array
            with a reason.
        expose_as_variable: For add_child_widget: mark the new widget as a
            Blueprint variable so other graphs can bind to it. Defaults True.
        save: Save the asset after the change. Defaults True.
        overwrite: For create: overwrite an existing asset at package_path.
            Defaults False.

    Returns:
        For set_slot_property: dict with ``slot_class`` /
        ``slot_class_path`` (the resolved UPanelSlot subclass), ``applied``
        (per-property name + reflected CPP type), ``skipped`` (per-property
        reason such as ``not_a_uproperty`` or ``import_text_failed`` plus
        the attempted ImportText input), ``applied_count`` /
        ``skipped_count``, and ``saved``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"operation": operation}
    if package_path is not None:
        params["package_path"] = package_path
    if widget_blueprint is not None:
        params["widget_blueprint"] = widget_blueprint
    if parent_class is not None:
        params["parent_class"] = parent_class
    if root_panel_class is not None:
        params["root_panel_class"] = root_panel_class
    if widget_type is not None:
        params["widget_type"] = widget_type
    if widget_name is not None:
        params["widget_name"] = widget_name
    if parent_name is not None:
        params["parent_name"] = parent_name
    if text is not None:
        params["text"] = text
    if properties is not None:
        params["properties"] = properties
    params["expose_as_variable"] = expose_as_variable
    params["save"] = save
    params["overwrite"] = overwrite

    try:
        response = unreal.send_command("widget_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"widget_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def editor_log(
    operation: str = "tail",
    lines: int = 200,
    category: Optional[str] = None,
    min_verbosity: Optional[str] = None,
    log_path: Optional[str] = None,
    message: Optional[str] = None,
    verbosity: Optional[str] = None,
) -> Dict[str, Any]:
    """
    Read or append an Output Log entry.

    Mirrors a small slice of the hosted Flop "editor_log" tool. The Unreal
    editor's Output Log is mirrored to disk at <Project>/Saved/Logs/<Project>.log,
    and that file is what we read. Writes go through GLog and a custom
    LogSproftMCP category so they show up in both the in-editor Output Log
    and the on-disk file.

    Args:
        operation: "tail" to read recent log lines, "write" to emit one.
        lines: For tail: max number of lines to return (1..5000). Default 200.
        category: For tail: optional category filter, e.g. "LogTemp".
        min_verbosity: For tail: optional minimum severity. One of fatal,
            error, warning, display, log, verbose, very_verbose.
        log_path: For tail: optional override for the log file path.
        message: For write: the line of text to log. Required for write.
        verbosity: For write: severity. Defaults to "log". "fatal" is
            demoted to "error" so the editor does not crash.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"operation": operation}
    if lines is not None:
        params["lines"] = lines
    if category is not None:
        params["category"] = category
    if min_verbosity is not None:
        params["min_verbosity"] = min_verbosity
    if log_path is not None:
        params["log_path"] = log_path
    if message is not None:
        params["message"] = message
    if verbosity is not None:
        params["verbosity"] = verbosity

    try:
        response = unreal.send_command("editor_log", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"editor_log error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_input(
    operation: str,
    package_path: Optional[str] = None,
    value_type: Optional[str] = None,
    description: Optional[str] = None,
    trigger_when_paused: bool = False,
    input_mapping_context: Optional[str] = None,
    input_action: Optional[str] = None,
    key: Optional[str] = None,
    blueprint: Optional[str] = None,
    trigger: Optional[str] = None,
    connect_to_function: Optional[str] = None,
    position: Optional[List[float]] = None,
    compile: bool = True,
    save: bool = True,
    overwrite: bool = False,
) -> Dict[str, Any]:
    """
    Manage Enhanced Input data assets and wire input events into Blueprints.

    Mirrors a slice of the hosted Flop "bp_input" tool. Four operations:

        - "create_input_action": create a UInputAction asset with a chosen
          value type (Boolean, Axis1D, Axis2D, Axis3D).
        - "create_input_mapping_context": create an empty UInputMappingContext.
        - "add_mapping": append one key-to-action binding to an existing IMC.
        - "add_action_event_node": spawn a UK2Node_EnhancedInputAction event
          node in a target Blueprint's event graph for a given UInputAction
          asset. Optionally MakeLinkTo from the chosen trigger exec pin
          (default "Triggered") to a named function call on the same
          Blueprint.

    Args:
        operation: "create_input_action", "create_input_mapping_context",
            "add_mapping", or "add_action_event_node".
        package_path: For create operations: the absolute content-browser path
            for the new asset, e.g. "/Game/Input/IA_Jump".
        value_type: For create_input_action: one of "bool" / "boolean",
            "axis1d" / "float", "axis2d" / "vector2d", "axis3d" / "vector".
            Defaults to "bool".
        description: Optional ActionDescription / ContextDescription text.
        trigger_when_paused: For create_input_action: allow the action to
            trigger while the game is paused. Defaults False.
        input_mapping_context: For add_mapping: absolute path to the existing
            UInputMappingContext asset.
        input_action: For add_mapping / add_action_event_node: absolute path
            to the existing UInputAction asset.
        key: For add_mapping: FKey FName like "SpaceBar", "W",
            "Gamepad_FaceButton_Bottom", or "Gamepad_LeftStick_X".
        blueprint: For add_action_event_node: absolute path to the target
            Blueprint asset whose event graph will receive the node.
        trigger: For add_action_event_node: name of the exec pin to wire,
            mirroring ETriggerEvent enum names. One of "Triggered" (default),
            "Started", "Ongoing", "Canceled", "Completed".
        connect_to_function: For add_action_event_node: optional FName of an
            existing function on the target Blueprint. The trigger exec pin
            is MakeLinkTo'd to a CallFunction node for it.
        position: For add_action_event_node: optional [x, y] node-graph
            coordinate. Defaults [0, 0].
        compile: For add_action_event_node: compile the Blueprint after
            placing the node. Defaults True.
        save: Save the asset(s) after the change. Defaults True.
        overwrite: For create operations: overwrite an existing asset at
            package_path. Defaults False.

    Returns:
        Dictionary with operation-specific metadata on success.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"operation": operation}
    if package_path is not None:
        params["package_path"] = package_path
    if value_type is not None:
        params["value_type"] = value_type
    if description is not None:
        params["description"] = description
    params["trigger_when_paused"] = trigger_when_paused
    if input_mapping_context is not None:
        params["input_mapping_context"] = input_mapping_context
    if input_action is not None:
        params["input_action"] = input_action
    if key is not None:
        params["key"] = key
    if blueprint is not None:
        params["blueprint"] = blueprint
    if trigger is not None:
        params["trigger"] = trigger
    if connect_to_function is not None:
        params["connect_to_function"] = connect_to_function
    if position is not None:
        params["position"] = position
    params["compile"] = compile
    params["save"] = save
    params["overwrite"] = overwrite

    try:
        response = unreal.send_command("bp_input", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_input error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def widget_inspect(
    widget_blueprint: str,
    include_variables: bool = True,
    include_functions: bool = True,
    include_flat: bool = True,
    include_named_slots: bool = True,
) -> Dict[str, Any]:
    """
    Read-only dump of a Widget Blueprint's tree, variables, and functions.

    Mirrors the read side of the hosted Flop "widget_inspect" tool. The
    existing read_blueprint_content tool returns an empty components list
    for Widget Blueprints because UMG widgets do not live in a
    SimpleConstructionScript; this tool fills that gap by walking the
    UWidgetTree directly.

    Args:
        widget_blueprint: Absolute content-browser path to the
            UWidgetBlueprint, e.g. "/Game/UI/WBP_CraftingMenu".
        include_variables: Include the asset's NewVariables list, minus
            entries that are also widget tree members.
        include_functions: Include a short list of FunctionGraphs.
        include_flat: Include a flat list of every widget in the tree, not
            just the nested form.
        include_named_slots: Include any UNamedSlot widgets exposed for
            content injection.

    Returns:
        Dictionary with widget_blueprint, name, parent_class, tree, widgets,
        widget_count, named_slots, variables, functions.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "operation": "inspect",
        "widget_blueprint": widget_blueprint,
        "include_variables": include_variables,
        "include_functions": include_functions,
        "include_flat": include_flat,
        "include_named_slots": include_named_slots,
    }

    try:
        response = unreal.send_command("widget_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"widget_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_component(
    blueprint: str,
    component_class: str,
    component_name: str,
    parent_component: Optional[str] = None,
    properties: Optional[Dict[str, Any]] = None,
    location: Optional[List[float]] = None,
    rotation: Optional[List[float]] = None,
    scale: Optional[List[float]] = None,
    compile: bool = True,
    save: bool = True,
) -> Dict[str, Any]:
    """
    Add a component to an existing Blueprint's SimpleConstructionScript.

    Mirrors the small "add a component" cut of the hosted Flop "bp_component"
    tool. The class resolver accepts short names (StaticMeshComponent,
    SpringArm, CameraComponent), with or without the "U" prefix and / or the
    "Component" suffix, plus full /Script/Module.ClassName paths and BP class
    paths under /Game/. The optional parent_component name attaches the new
    node under an existing scene-component SCS node; otherwise the node is
    added at the SCS root.

    Args:
        blueprint: Short name or absolute /Game/ path of the target
            UBlueprint asset.
        component_class: Component class to instantiate. Examples:
            "StaticMeshComponent", "USphereComponent", "CameraComponent",
            "SpringArm", "/Script/Engine.PointLightComponent".
        component_name: Variable name for the new SCS node.
        parent_component: Optional FName of an existing scene-component SCS
            node to attach under. Defaults to the root.
        properties: Optional flat dict of property names to JSON values
            applied through FProperty::ImportText on the component template.
        location: Optional [x, y, z] relative location for scene components.
        rotation: Optional [pitch, yaw, roll] relative rotation.
        scale: Optional [x, y, z] relative scale.
        compile: Compile the Blueprint after the change. Defaults True.
        save: Save the Blueprint asset after compile. Defaults True.

    Returns:
        Dictionary with operation, blueprint, component_name, component_class,
        is_scene_component, optional parent_component, attached_as_root,
        applied / skipped property lists, compiled, saved.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "operation": "add_component",
        "blueprint": blueprint,
        "component_class": component_class,
        "component_name": component_name,
        "compile": compile,
        "save": save,
    }
    if parent_component is not None:
        params["parent_component"] = parent_component
    if properties is not None:
        params["properties"] = properties
    if location is not None:
        params["location"] = location
    if rotation is not None:
        params["rotation"] = rotation
    if scale is not None:
        params["scale"] = scale

    try:
        response = unreal.send_command("bp_component", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_component error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def scene_query(
    actor_class: Optional[str] = None,
    match_class_substring: bool = True,
    name_pattern: Optional[str] = None,
    label_pattern: Optional[str] = None,
    tag: Optional[str] = None,
    center: Optional[List[float]] = None,
    radius: Optional[float] = None,
    limit: int = 256,
) -> Dict[str, Any]:
    """
    Read-only multiplexed actor query for the editor world.

    Mirrors a small cut of the hosted Flop "scene_query" tool. All filters
    are optional and combine with AND. Returns name / label / class /
    location / rotation / scale / tags / mobility / hidden flags for each
    match, plus a total / returned / truncated counters.

    Args:
        actor_class: Filter by class. By default this is a case-insensitive
            substring match against both the short class name and the full
            object path (e.g. "StaticMesh" matches AStaticMeshActor and
            UStaticMeshComponent owners). Set match_class_substring=False
            for an exact short-name match (e.g. "StaticMeshActor").
        match_class_substring: When True (default), actor_class is treated
            as a substring. When False, actor_class is resolved to a UClass
            and GetAllActorsOfClass narrows the world walk.
        name_pattern: Case-insensitive substring filter against GetName().
        label_pattern: Case-insensitive substring filter against
            GetActorLabel() (the Outliner label).
        tag: A single FName actor tag the actor must carry.
        center: Optional [x, y, z] world-space centre for the spatial filter.
        radius: Optional sphere radius (cm) for the spatial filter. Both
            center and radius must be supplied together.
        limit: Cap on returned records. Defaults 256.

    Returns:
        Dictionary with actors (list), total_matches, returned, truncated,
        limit.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "match_class_substring": match_class_substring,
        "limit": limit,
    }
    if actor_class is not None:
        params["class"] = actor_class
    if name_pattern is not None:
        params["name_pattern"] = name_pattern
    if label_pattern is not None:
        params["label_pattern"] = label_pattern
    if tag is not None:
        params["tag"] = tag
    if center is not None:
        params["center"] = center
    if radius is not None:
        params["radius"] = radius

    try:
        response = unreal.send_command("scene_query", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"scene_query error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def material_edit(
    operation: str,
    package_path: Optional[str] = None,
    base_color: Optional[List[float]] = None,
    parent_material: Optional[str] = None,
    material_instance: Optional[str] = None,
    parameter_name: Optional[str] = None,
    parameter_type: Optional[str] = None,
    value: Optional[Any] = None,
    save: bool = True,
    overwrite: bool = False,
    material: Optional[str] = None,
    expression_class: Optional[str] = None,
    position: Optional[List[float]] = None,
    properties: Optional[Dict[str, Any]] = None,
    property: Optional[str] = None,
    connect_to: Optional[str] = None,
    connect_input: Optional[str] = None,
    source: Optional[str] = None,
    source_output: Optional[str] = None,
    dest: Optional[str] = None,
    dest_input: Optional[str] = None,
    expression: Optional[str] = None,
    expressions: Optional[List[Dict[str, Any]]] = None,
    connections: Optional[List[Dict[str, Any]]] = None,
    recompile: bool = True,
) -> Dict[str, Any]:
    """
    Material authoring. Supports asset creation, instance overrides, and
    MaterialExpression-graph editing on a UMaterial.

    Operations:

        - "create_material": create a UMaterial with an optional Constant3
          base-colour driver wired into the BaseColor input.
        - "create_material_instance_constant": create a Material Instance
          Constant pointing at a parent UMaterial / UMaterialInstance.
        - "set_instance_parameter": override scalar / vector / texture
          parameters on a Material Instance Constant.
        - "add_expression": append a UMaterialExpression to a UMaterial.
          Resolves the class from a short name ("multiply", "lerp",
          "scalar_parameter", "texture_sample_parameter_2d", "time",
          "panner", "constant", "constant3vector", "vector_parameter",
          "one_minus", "saturate", "clamp", "fresnel", "power", "sine",
          "cosine", "component_mask", "if", "make_material_attributes"),
          a full ``/Script/Engine.UMaterialExpressionFoo`` path, or a
          bare ``MaterialExpressionFoo`` class name. ``properties``
          flat dict applies through ``FProperty::ImportText`` so callers
          can land ``ConstA``, ``ConstB``, ``ParameterName``,
          ``DefaultValue`` etc. on creation. Optional one-shot
          connection: ``property`` connects the new expression to a
          material attribute (BaseColor, EmissiveColor, etc.); or
          ``connect_to`` + ``connect_input`` connects it into another
          named expression's input pin.
        - "connect_expressions": connect a source expression to either a
          material attribute (``property``) or another expression
          (``dest`` + ``dest_input``). ``source_output`` defaults to the
          primary output.
        - "set_expression_property": apply a flat property dict to a
          named expression on the material (e.g. tweak ``ConstA`` on a
          Multiply or ``ParameterName`` on a Scalar Parameter).
        - "add_expressions": bulk variant. Takes ``expressions`` (list
          of expression specs, each with ``class`` plus optional
          ``name`` alias / ``position`` / ``properties`` dict) and an
          optional ``connections`` list (each entry either
          ``{source, source_output?, dest, dest_input?}`` between
          expressions or ``{source, property}`` to a material
          attribute). Uses each spec's ``name`` as a friendly alias so
          a connection can reference an expression created earlier in
          the same call without waiting for the engine's resolved FName
          to come back. Recompiles + saves once after the whole batch.

    Material Functions and Material Parameter Collections remain on the
    backlog.

    Args:
        operation: One of the operation names listed above.
        package_path: For create operations: absolute /Game/ asset path.
        base_color: For create_material: [r, g, b(, a)] linear colour wired
            into BaseColor through a Constant3Vector expression.
        parent_material: For create_material_instance_constant: parent
            material asset path.
        material_instance: For set_instance_parameter: target Material
            Instance Constant path.
        parameter_name: Parameter FName for set_instance_parameter.
        parameter_type: Optional "scalar" / "vector" / "color" / "texture"
            hint for set_instance_parameter.
        value: Parameter value for set_instance_parameter.
        save: Save the asset after the change. Defaults True.
        overwrite: For create operations: overwrite existing assets.
        material: Target UMaterial path for the expression-graph ops
            (add_expression, connect_expressions, set_expression_property).
        expression_class: Short name, full /Script path, or bare class
            name for add_expression.
        position: Optional [x, y] override for the new expression's
            graph position. When omitted, the position cascades down so
            stacked nodes do not pile up.
        properties: Flat property dict applied through
            ``FProperty::ImportText``. Used by add_expression and
            set_expression_property.
        property: For add_expression / connect_expressions: target
            material attribute (BaseColor, Roughness, etc.).
        connect_to: For add_expression: another named expression on the
            material to connect into.
        connect_input: For add_expression / connect_expressions:
            destination input name on the connected expression.
        source: For connect_expressions: source expression FName.
        source_output: Output pin name on the source expression
            (default: primary output).
        dest: For connect_expressions: destination expression FName.
        dest_input: Input pin name on the destination expression.
        expression: For set_expression_property: target expression FName.
        expressions: For add_expressions: list of expression specs.
            Each spec is ``{class, name?, position?, properties?}``.
            ``name`` is a friendly alias so connection rows can refer
            back to it within the same call.
        connections: For add_expressions: list of edge specs. Each is
            either ``{source, source_output?, dest, dest_input?}`` or
            ``{source, property}`` (to a material attribute).
        recompile: For expression-graph ops: recompile the material on
            success. Defaults True.

    Returns:
        Dictionary with operation-specific metadata on success.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"operation": operation, "save": save, "overwrite": overwrite}
    if package_path is not None:
        params["package_path"] = package_path
    if base_color is not None:
        params["base_color"] = base_color
    if parent_material is not None:
        params["parent_material"] = parent_material
    if material_instance is not None:
        params["material_instance"] = material_instance
    if parameter_name is not None:
        params["parameter_name"] = parameter_name
    if parameter_type is not None:
        params["parameter_type"] = parameter_type
    if value is not None:
        params["value"] = value
    if material is not None:
        params["material"] = material
    if expression_class is not None:
        params["class"] = expression_class
    if position is not None:
        params["position"] = position
    if properties is not None:
        params["properties"] = properties
    if property is not None:
        params["property"] = property
    if connect_to is not None:
        params["connect_to"] = connect_to
    if connect_input is not None:
        params["connect_input"] = connect_input
    if source is not None:
        params["source"] = source
    if source_output is not None:
        params["source_output"] = source_output
    if dest is not None:
        params["dest"] = dest
    if dest_input is not None:
        params["dest_input"] = dest_input
    if expression is not None:
        params["expression"] = expression
    if expressions is not None:
        params["expressions"] = expressions
    if connections is not None:
        params["connections"] = connections
    if recompile is False:
        # The C++ default is true; only forward when caller wants false.
        params["recompile"] = False

    try:
        response = unreal.send_command("material_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"material_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def actor_inspect(
    actor: str,
    include_components: bool = True,
    include_component_properties: bool = False,
    include_actor_properties: bool = False,
    max_properties_per_object: int = 24,
) -> Dict[str, Any]:
    """
    Read-only single-actor dump with components and key properties.

    Mirrors the read side of the hosted Flop "actor_inspect" tool. The actor
    is resolved by FName first and then by case-insensitive Outliner label.
    Returns the actor's transform, tags, replication snapshot, root component,
    and an optional list of all attached components, each with their class,
    relative transform, attach parent / socket, tags, and (when requested) a
    short uproperty value dump rendered through FProperty::ExportText.

    Args:
        actor: Actor name (FName from GetName()) or Outliner label.
        include_components: Include the components list. Defaults True.
        include_component_properties: For each component, include a small
            uproperty value dump capped by max_properties_per_object.
            Defaults False to keep responses compact.
        include_actor_properties: Include the actor-level uproperty dump.
            Defaults False.
        max_properties_per_object: Per-object cap on uproperty entries
            emitted when properties are included. Defaults 24.

    Returns:
        Dictionary with actor metadata, components list (when requested),
        and optional actor / component property dumps.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "operation": "inspect",
        "actor": actor,
        "include_components": include_components,
        "include_component_properties": include_component_properties,
        "include_actor_properties": include_actor_properties,
        "max_properties_per_object": max_properties_per_object,
    }

    try:
        response = unreal.send_command("actor_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"actor_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def scene_compose(
    operation: str,
    actor_class: Optional[str] = None,
    actor: Optional[str] = None,
    name: Optional[str] = None,
    label: Optional[str] = None,
    location: Optional[List[float]] = None,
    rotation: Optional[List[float]] = None,
    scale: Optional[List[float]] = None,
    tags: Optional[List[str]] = None,
    properties: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """
    Declarative single-actor scene mutation: spawn, modify, or delete.

    Mirrors the small cut of the hosted Flop "scene_compose" tool. One actor
    per call so each request stays auditable. Spawn returns the resolved
    name of the new actor in the response.

    Operations:
        - "spawn": create an actor of actor_class at the given transform
          with optional preferred name, Outliner label, tags, and a flat
          property dict. Returns the new actor record.
        - "modify": apply a partial transform / label / tags / property
          patch to an existing actor resolved by name or label. Each axis
          of the transform is independent; missing axes leave the existing
          component untouched.
        - "delete": destroy an existing actor resolved by name or label.

    Args:
        operation: "spawn", "modify", or "delete".
        actor_class: For spawn: an Engine class short name (e.g.
            "StaticMeshActor", "PointLight") or a full path. /Game/-rooted
            BP class paths are loaded with or without the "_C" suffix.
        actor: For modify / delete: actor name (FName) or Outliner label.
        name: For spawn: optional preferred FName. Rejected with an error
            if a sibling actor already uses it.
        label: For spawn: optional Outliner label. For modify: replacement
            Outliner label.
        location: For spawn: world-space [x, y, z] location. For modify:
            replacement location. Cm.
        rotation: For spawn: [pitch, yaw, roll]. For modify: replacement
            rotation.
        scale: For spawn: [x, y, z] scale. For modify: replacement scale.
        tags: For spawn / modify: optional full replacement list of FName
            actor tags.
        properties: For spawn / modify: optional flat dict of property
            names to JSON values applied through FProperty::ImportText on
            the actor instance.

    Returns:
        Dictionary with the operation, the affected actor record (spawn /
        modify), and applied / skipped property lists.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"operation": operation}
    if actor_class is not None:
        params["class"] = actor_class
    if actor is not None:
        params["actor"] = actor
    if name is not None:
        params["name"] = name
    if label is not None:
        params["label"] = label
    if location is not None:
        params["location"] = location
    if rotation is not None:
        params["rotation"] = rotation
    if scale is not None:
        params["scale"] = scale
    if tags is not None:
        params["tags"] = tags
    if properties is not None:
        params["properties"] = properties

    try:
        response = unreal.send_command("scene_compose", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"scene_compose error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def python_execution(
    operation: str,
    code: Optional[str] = None,
    path: Optional[str] = None,
    args: Optional[List[str]] = None,
    mode: Optional[str] = None,
    unattended: bool = True,
    max_response_chars: int = 65536,
) -> Dict[str, Any]:
    """
    Run Python in the editor's interpreter through PythonScriptPlugin.

    Mirrors the hosted Flop "python_execution" tool. Two operations:

        - "execute_string": run a string of Python source. The default mode
          is "execute_file" so multi-statement programs and `import`
          statements behave as if you ran them from a file.
        - "execute_file": run a Python file on disk. The path may include
          positional `args`; the engine forwards them as `sys.argv[1:]`.

    Returns the captured stdout / stderr lines, the boolean success flag
    `ok`, and `command_result` (the repr of the last evaluated expression
    for evaluate-statement mode, None otherwise). Output is truncated at
    max_response_chars to keep the JSON channel responsive.

    The PythonScriptPlugin must be enabled in the consumer project. The
    UnrealMCP plugin manifest references it, so projects that depend on
    UnrealMCP pull it in automatically; if it is disabled the tool returns
    a structured error message.

    Args:
        operation: "execute_string" or "execute_file".
        code: For execute_string: the Python source.
        path: For execute_file: filesystem path to a .py file. Must exist.
        args: For execute_file: optional list of positional arguments
            forwarded to the file as `sys.argv[1:]`.
        mode: Optional override of the engine execution mode:
            "execute_file" (default), "execute_statement", or
            "evaluate_statement". Statement / evaluate modes are
            single-statement only.
        unattended: Pass `EPythonCommandFlags::Unattended`. Defaults True
            so dialogs and prompts do not block the editor while we run.
        max_response_chars: Cap on stdout / stderr / command_result size.
            Defaults 64 KiB.

    Returns:
        Dictionary with operation, mode, ok, stdout, stderr, command_result,
        log entries.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "operation": operation,
        "unattended": unattended,
        "max_response_chars": max_response_chars,
    }
    if code is not None:
        params["code"] = code
    if path is not None:
        params["path"] = path
    if args is not None:
        params["args"] = args
    if mode is not None:
        params["mode"] = mode

    try:
        response = unreal.send_command("python_execution", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"python_execution error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def scene_brief(
    max_class_counts: int = 256,
    max_notable_actors: int = 16,
) -> Dict[str, Any]:
    """
    Read-only one-shot orientation summary of the active editor world.

    Mirrors the documented "scene_brief" entry on the hosted Flop tool surface.
    The goal is a designer-readable snapshot without paying for a full
    per-actor list. One call returns level identity, sublevel list, actor
    count and per-class counts, world bounds union, current GameMode override
    plus default pawn class, the Level Blueprint's "has user events" flag,
    deduplicated tags in use, and a short list of notable actors (player
    starts, directional lights, post-process volumes).

    Args:
        max_class_counts: Cap on entries returned in class_counts. Defaults
            256. The remaining entries are reported through
            class_counts_truncated and unique_classes.
        max_notable_actors: Cap on entries returned in notable_actors.
            Defaults 16. Set to 0 to skip the list.

    Returns:
        Dictionary with level_name, level_path, sublevels, actor_count,
        class_counts, class_counts_truncated, unique_classes, tags_in_use,
        bounds (or None when nothing has bounds), game_mode_class,
        default_pawn_class, level_blueprint_has_events, notable_actors.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "max_class_counts": max_class_counts,
        "max_notable_actors": max_notable_actors,
    }

    try:
        response = unreal.send_command("scene_brief", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"scene_brief error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def level_inspect(
    actor_class: Optional[str] = None,
    match_class_substring: bool = True,
    name_pattern: Optional[str] = None,
    label_pattern: Optional[str] = None,
    tag: Optional[str] = None,
    level_filter: Optional[str] = None,
    include_components: bool = False,
    limit: int = 512,
) -> Dict[str, Any]:
    """
    Read-only structured per-actor record list for the editor world.

    Sits between scene_brief (one designer summary) and scene_query (filtered
    subset). Always returns a uniformly-shaped per-actor block, plus a
    per-level summary, so the agent can plan a level audit in one call. All
    filters are optional and combine with AND.

    Args:
        actor_class: Filter by class. Substring match by default; pass
            match_class_substring=False for an exact short-name match.
        match_class_substring: When True (default), actor_class is treated
            as a substring against the short class name and full class path.
        name_pattern: Case-insensitive substring filter against GetName().
        label_pattern: Case-insensitive substring filter against
            GetActorLabel() (the Outliner label).
        tag: A single FName actor tag the actor must carry.
        level_filter: Case-insensitive substring filter against the owning
            ULevel's owning-world name. Useful in worlds with sublevels.
        include_components: When True, emit a compact component list per
            actor (name, class, mobility, tags). Defaults False to keep
            responses compact.
        limit: Cap on returned actor records. Defaults 512.

    Returns:
        Dictionary with operation, level_name, level_path, levels (per-level
        summary), actors (per-actor records), actors_scanned, total_matches,
        returned, truncated, limit.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "match_class_substring": match_class_substring,
        "include_components": include_components,
        "limit": limit,
    }
    if actor_class is not None:
        params["class"] = actor_class
    if name_pattern is not None:
        params["name_pattern"] = name_pattern
    if label_pattern is not None:
        params["label_pattern"] = label_pattern
    if tag is not None:
        params["tag"] = tag
    if level_filter is not None:
        params["level_filter"] = level_filter

    try:
        response = unreal.send_command("level_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"level_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def tag_registry_edit(
    operation: str,
    tag: Optional[str] = None,
    comment: Optional[str] = None,
    source: Optional[str] = None,
    pattern: Optional[str] = None,
    only_dictionary_tags: bool = False,
    restricted: bool = False,
    allow_non_restricted_children: bool = True,
    limit: int = 256,
) -> Dict[str, Any]:
    """
    Manage the project's Gameplay Tag registry through the editor module.

    Three operations:

      - "add_tag": writes a tag (and optional dev comment) into a chosen
        ``Config/Default*Tags.ini`` file. Defaults to ``DefaultGameplayTags.ini``.
        The editor module rewrites the ini, refreshes the in-memory tag tree,
        and broadcasts the change so live tag pickers refresh.
      - "remove_tag": deletes a tag from the ini that owns it. Children, if
        any, get a redirector entry written automatically by the editor module.
      - "list_tags": read-only substring search over all registered tags.
        Returns each tag's name, owning source, source ini path (when the
        source is config-backed), and dev comment.

    Args:
        operation: One of ``add_tag`` / ``remove_tag`` / ``list_tags``.
        tag: The fully-qualified tag string for ``add_tag`` and ``remove_tag``,
            for example ``Status.Damage.Fire``.
        comment: Optional developer comment, applied on ``add_tag``.
        source: Optional FName of the tag source (typically an ini filename
            such as ``DefaultGameplayTags.ini``). Defaults to that value when
            unset on ``add_tag``.
        pattern: Optional case-insensitive substring filter for ``list_tags``.
        only_dictionary_tags: When True, ``list_tags`` excludes implicitly
            added tags. Defaults False.
        restricted: When True, ``add_tag`` writes a restricted tag.
        allow_non_restricted_children: For restricted ``add_tag`` writes,
            whether normal children are still allowed under this tag.
            Defaults True.
        limit: Cap on returned entries from ``list_tags``. Defaults 256.

    Returns:
        Operation-specific dict; see C++ handler for the exact shape.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"operation": operation}
    if tag is not None:
        params["tag"] = tag
    if comment is not None:
        params["comment"] = comment
    if source is not None:
        params["source"] = source
    if pattern is not None:
        params["pattern"] = pattern
    params["only_dictionary_tags"] = only_dictionary_tags
    params["restricted"] = restricted
    params["allow_non_restricted_children"] = allow_non_restricted_children
    params["limit"] = limit

    try:
        response = unreal.send_command("tag_registry_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"tag_registry_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_create(
    name: str,
    parent_class: Optional[str] = None,
    path: Optional[str] = None,
    properties: Optional[Dict[str, Any]] = None,
    compile: bool = True,
    save: bool = True,
    overwrite: bool = False,
) -> Dict[str, Any]:
    """
    Create a UBlueprint asset with a chosen parent class.

    Wider surface than the local ``create_blueprint`` helper:

      - ``parent_class`` accepts a short name (Actor, Pawn, Character,
        ActorComponent, SceneComponent, GameMode, GameModeBase,
        PlayerController, AIController, UserWidget, DataAsset,
        BlueprintFunctionLibrary, etc.), a full ``/Script/Module.ClassName``
        path, or a ``/Game/...`` Blueprint class path.
      - ``path`` configures the output package path (``/Game/...``);
        defaults to ``/Game/Blueprints``.
      - ``properties`` is a flat dict applied via ``FProperty::ImportText``
        on the generated CDO before the first compile, so callers can land
        defaults in one shot.
      - Compiles and saves on success by default.

    Args:
        name: Short asset name. Must contain no invalid object characters.
        parent_class: Parent UClass, by short name or full path.
            Defaults to ``Actor``.
        path: ``/Game/...`` package path. Defaults to ``/Game/Blueprints``.
        properties: Optional flat dict applied to the generated CDO.
        compile: Compile the Blueprint after creation. Defaults True.
        save: Save the asset after compile. Defaults True.
        overwrite: When True, opens and reuses an existing asset at the
            target path; when False, fails if the asset already exists.

    Returns:
        Dictionary with operation, name, path, object_path, parent_class,
        parent_class_short, created, reused_existing, compiled, saved,
        applied (per-property report), and skipped (per-property report).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "operation": "create",
        "name": name,
        "compile": compile,
        "save": save,
        "overwrite": overwrite,
    }
    if parent_class is not None:
        params["parent_class"] = parent_class
    if path is not None:
        params["path"] = path
    if properties is not None:
        params["properties"] = properties

    try:
        response = unreal.send_command("bp_create", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_create error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_brief(blueprint: str) -> Dict[str, Any]:
    """
    Read-only one-page orientation summary of a Blueprint asset.

    Smaller and faster than ``read_blueprint_content`` plus
    ``analyze_blueprint_graph``. Useful when the agent just needs to know
    "what kind of BP is this" before a deeper pass.

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.

    Returns:
        Dictionary with name, path, package_name, parent_class,
        parent_class_short, blueprint_type, variable_count, function_count,
        macro_count, event_graph_node_count, events (list of named events),
        components (list of {name, class, is_root}), component_count,
        interfaces (list of {name, path}), and is_data_only.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        response = unreal.send_command("bp_brief", {"blueprint": blueprint})
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_brief error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_inspect(
    op: str,
    blueprint: str,
    class_pattern: Optional[str] = None,
    title_pattern: Optional[str] = None,
    pattern: Optional[str] = None,
    limit: int = 64,
) -> Dict[str, Any]:
    """
    Read-only targeted query operations on a Blueprint asset.

    Sits between ``bp_brief`` (one-shot orientation) and
    ``read_blueprint_content`` (full dump). Each op returns a focused
    payload so the agent can answer specific questions without authoring
    Python every time.

    Operations:

      - ``list_variables``: typed variables with default value, edit
        flags, category, and friendly name.
      - ``list_functions``: user-authored function and macro graphs with
        node counts.
      - ``list_events``: event-graph events (UK2Node_Event +
        UK2Node_CustomEvent), with the owning ubergraph name.
      - ``list_components``: SCS components with class, scene/actor
        flag, attach parent, attach socket, root flag, and child count.
      - ``find_node``: node search across all graphs by case-insensitive
        substring against either node short class name (``class_pattern``)
        or node title (``title_pattern``). Pass ``pattern`` to match either.

    Args:
        op: One of ``list_variables`` / ``list_functions`` / ``list_events``
            / ``list_components`` / ``find_node``.
        blueprint: Short name or full ``/Game/...`` Blueprint path.
        class_pattern: Substring against node class short name (find_node).
        title_pattern: Substring against node title (find_node).
        pattern: Convenience: matches either class_pattern or title_pattern.
        limit: Cap on returned matches in find_node. Defaults 64.

    Returns:
        Operation-specific dict; see C++ handler for the exact shape.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"op": op, "blueprint": blueprint, "limit": limit}
    if class_pattern is not None:
        params["class_pattern"] = class_pattern
    if title_pattern is not None:
        params["title_pattern"] = title_pattern
    if pattern is not None:
        params["pattern"] = pattern

    try:
        response = unreal.send_command("bp_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_variable(
    op: str,
    blueprint: str,
    name: Optional[str] = None,
    type: Optional[str] = None,
    value_type: Optional[str] = None,
    container: Optional[str] = None,
    default: Any = None,
    category: Optional[str] = None,
    friendly_name: Optional[str] = None,
    tooltip: Optional[str] = None,
    editable: Optional[bool] = None,
    blueprint_read_only: Optional[bool] = None,
    blueprint_writable: Optional[bool] = None,
    expose_on_spawn: Optional[bool] = None,
    replicated: Optional[bool] = None,
    expose_to_cinematics: Optional[bool] = None,
    instance_editable: Optional[bool] = None,
    private: Optional[bool] = None,
    flags: Optional[Dict[str, bool]] = None,
    compile: bool = True,
    save: bool = True,
) -> Dict[str, Any]:
    """
    Declarative Blueprint variable management.

    Sits next to the local ``create_variable`` and
    ``set_blueprint_variable_properties`` helpers, but with a single
    multi-op surface and a wider type resolver.

    Operations (``op``):

      - ``list``: dump every Blueprint variable with type, current
        default, category, friendly name, and the user-visible flag set.
      - ``add``: declare a new variable. Required: ``name``, ``type``.
        Optional: ``container`` (``single`` / ``array`` / ``set`` /
        ``map``), ``value_type`` (required when ``container=map``),
        ``default``, ``category``, ``friendly_name``, ``tooltip``, plus
        any of the flag toggles below.
      - ``remove``: delete a variable by ``name``.
      - ``set_default``: overwrite the variable's default value with the
        JSON ``default`` payload (string / number / bool / list / dict).
      - ``set_flags``: mutate the variable's flag set via the ``flags``
        dict. Recognised keys: editable, blueprint_read_only,
        blueprint_writable, instance_editable, expose_on_spawn,
        replicated, expose_to_cinematics, private.

    Type tokens accepted by ``type`` and ``value_type``:

      - Scalars: bool / int / int64 / byte / float / double / string /
        name / text.
      - Built-in structs: vector / vector2d / rotator / transform /
        color / linear_color.
      - Object refs: full ``/Script/Module.ClassName`` path.
      - Blueprint class refs: ``/Game/...`` Blueprint asset path; the
        resolver appends ``_C`` automatically.
      - User structs: ``struct:/Game/Path/MyStruct`` or
        ``struct:/Script/Module.MyStruct``.

    Args:
        op: One of ``list`` / ``add`` / ``remove`` / ``set_default`` /
            ``set_flags``.
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        name: Variable FName (required for non-``list`` ops).
        type: Type token for the variable (required for ``add``).
        value_type: Map value type token (required when ``container=map``).
        container: Container type. Defaults to ``single``.
        default: JSON value applied as the variable's default.
        category, friendly_name, tooltip: Optional metadata.
        editable, blueprint_read_only, blueprint_writable, expose_on_spawn,
            replicated, expose_to_cinematics, instance_editable, private:
            Optional flag toggles. Pass these on ``add`` to land flags at
            creation time, or use ``set_flags`` with a ``flags`` dict.
        flags: Dict of flag toggles for the ``set_flags`` op.
        compile: Compile the Blueprint after each mutating op. Defaults True.
        save: Save the asset after compile. Defaults True.

    Returns:
        Operation-specific dict; see the C++ handler for the exact shape.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "op": op,
        "blueprint": blueprint,
        "compile": compile,
        "save": save,
    }
    if name is not None:
        params["name"] = name
    if type is not None:
        params["type"] = type
    if value_type is not None:
        params["value_type"] = value_type
    if container is not None:
        params["container"] = container
    if default is not None:
        params["default"] = default
    if category is not None:
        params["category"] = category
    if friendly_name is not None:
        params["friendly_name"] = friendly_name
    if tooltip is not None:
        params["tooltip"] = tooltip
    if editable is not None:
        params["editable"] = editable
    if blueprint_read_only is not None:
        params["blueprint_read_only"] = blueprint_read_only
    if blueprint_writable is not None:
        params["blueprint_writable"] = blueprint_writable
    if expose_on_spawn is not None:
        params["expose_on_spawn"] = expose_on_spawn
    if replicated is not None:
        params["replicated"] = replicated
    if expose_to_cinematics is not None:
        params["expose_to_cinematics"] = expose_to_cinematics
    if instance_editable is not None:
        params["instance_editable"] = instance_editable
    if private is not None:
        params["private"] = private
    if flags is not None:
        params["flags"] = flags

    try:
        response = unreal.send_command("bp_variable", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_variable error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_class(
    op: str,
    blueprint: str,
    parent_class: Optional[str] = None,
    description: Optional[str] = None,
    display_name: Optional[str] = None,
    namespace: Optional[str] = None,
    category: Optional[str] = None,
    hide_categories: Optional[List[str]] = None,
    interface: Optional[str] = None,
    preserve_functions: Optional[bool] = None,
    compile: bool = True,
    save: bool = True,
) -> Dict[str, Any]:
    """
    Manage class-level settings on an existing UBlueprint asset.

    Sits next to ``bp_create`` (which creates the asset) and ``bp_brief``
    (which reads class metadata for orientation).

    Operations (``op``):

      - ``read``: dump current class-level settings: parent class,
        blueprint type, display name, description, namespace, category,
        hide categories list, and the implemented Blueprint interface
        paths.
      - ``set_parent``: re-parent the Blueprint to a new parent class.
        ``parent_class`` accepts a short name (Actor, Pawn, Character,
        ActorComponent, SceneComponent, GameMode, GameModeBase,
        PlayerController, AIController, UserWidget, DataAsset,
        BlueprintFunctionLibrary, etc.), a full
        ``/Script/Module.ClassName`` path, or a ``/Game/...`` Blueprint
        class path. Goes through ``Blueprint->ParentClass``,
        ``RefreshAllNodes``, and a recompile.
      - ``set_class_settings``: write any subset of ``description``,
        ``display_name``, ``namespace``, ``category``, and
        ``hide_categories`` (list, replaces the whole array) onto the
        Blueprint.
      - ``add_interface``: implement a Blueprint Interface. ``interface``
        accepts a full ``/Script/Module.IName`` path, a ``/Game/...``
        Blueprint Interface path, or a short name resolved against the
        loaded class set.
      - ``remove_interface``: tear down an implemented interface.
        ``preserve_functions`` defaults to False (drops the function
        graphs wholesale).

    Args:
        op: One of ``read`` / ``set_parent`` / ``set_class_settings`` /
            ``add_interface`` / ``remove_interface``.
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        parent_class: New parent class spec for ``set_parent``.
        description, display_name, namespace, category: Class-level
            string settings for ``set_class_settings``.
        hide_categories: List of categories to hide on instances of this
            Blueprint. Replaces the full HideCategories array.
        interface: Interface path for ``add_interface`` / ``remove_interface``.
        preserve_functions: When removing an interface, keep the
            function graphs as standalone graphs.
        compile: Compile the Blueprint after each mutating op. Defaults True.
        save: Save the asset after compile. Defaults True.

    Returns:
        Operation-specific dict; see the C++ handler for the exact shape.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "op": op,
        "blueprint": blueprint,
        "compile": compile,
        "save": save,
    }
    if parent_class is not None:
        params["parent_class"] = parent_class
    if description is not None:
        params["description"] = description
    if display_name is not None:
        params["display_name"] = display_name
    if namespace is not None:
        params["namespace"] = namespace
    if category is not None:
        params["category"] = category
    if hide_categories is not None:
        params["hide_categories"] = hide_categories
    if interface is not None:
        params["interface"] = interface
    if preserve_functions is not None:
        params["preserve_functions"] = preserve_functions

    try:
        response = unreal.send_command("bp_class", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_class error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_graph(
    op: str,
    blueprint: str,
    graph: Optional[str] = None,
    node: Optional[str] = None,
    class_pattern: Optional[str] = None,
    title_pattern: Optional[str] = None,
    include_exec: Optional[bool] = None,
    include_data: Optional[bool] = None,
    limit: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Read-only graph traversal beyond ``bp_inspect``.

    Useful when the agent needs to debug node wiring without running
    ``analyze_blueprint_graph`` over the whole asset.

    Operations (``op``):

      - ``list_graphs``: every graph on the Blueprint, grouped by kind
        (ubergraph / function / macro / interface). Returns name, kind,
        graph class, and node count per entry.
      - ``list_nodes``: every node in a chosen ``graph``. Returns node
        FName, short class, full title, position, and pin count. Optional
        ``class_pattern`` / ``title_pattern`` filters narrow the result.
        Defaults to a 256-node cap.
      - ``get_node``: full pin readback for one ``node`` in a chosen
        ``graph``. Returns each pin's name, direction, type description,
        default value, and connected target nodes / pins.
      - ``list_connections``: flat edge list for a chosen ``graph``.
        Each edge records source/target node FName + title + pin name.
        ``include_exec`` and ``include_data`` toggle filter edges by
        whether the source pin is exec or data. Defaults both True with
        a 1024-edge cap.

    Args:
        op: One of ``list_graphs`` / ``list_nodes`` / ``get_node`` /
            ``list_connections``.
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        graph: Graph FName resolved case-insensitively, with substring
            fallback. Required for non-``list_graphs`` ops.
        node: Node FName for ``get_node``. Resolved case-insensitively
            against the chosen graph, with title-substring fallback.
        class_pattern, title_pattern: Substring filters for ``list_nodes``.
        include_exec, include_data: Edge-kind filters for
            ``list_connections``. Default both True.
        limit: Cap on returned entries. Defaults 256 for ``list_nodes``
            and 1024 for ``list_connections``.

    Returns:
        Operation-specific dict; see the C++ handler for the exact shape.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"op": op, "blueprint": blueprint}
    if graph is not None:
        params["graph"] = graph
    if node is not None:
        params["node"] = node
    if class_pattern is not None:
        params["class_pattern"] = class_pattern
    if title_pattern is not None:
        params["title_pattern"] = title_pattern
    if include_exec is not None:
        params["include_exec"] = include_exec
    if include_data is not None:
        params["include_data"] = include_data
    if limit is not None:
        params["limit"] = limit

    try:
        response = unreal.send_command("bp_graph", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_graph error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_nodes(
    blueprint: str,
    nodes: Optional[List[Dict[str, Any]]] = None,
    node: Optional[Dict[str, Any]] = None,
    graph: Optional[str] = None,
    op: Optional[str] = None,
    compile: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Batched K2 node creation in a Blueprint graph.

    Spawns one or more nodes in a single call. The default graph is the
    Blueprint's first event graph; pass ``graph`` to target a specific
    function / macro / interface graph (resolved case-insensitively, with
    a substring fallback to match ``bp_graph``). Each node entry takes a
    ``class`` short name plus class-specific parameters; the response
    returns each new node's FName, GUID, position, and full pin list so
    a follow-up ``bp_wire`` call can address the pins by name.

    Compile is NOT automatic. Run ``compile_blueprint`` once after wiring
    up the nodes, or pass ``compile=True`` to force a compile after this
    batch.

    Supported ``class`` tokens:

      - ``variable_get`` (requires ``variable_name``)
      - ``variable_set`` (requires ``variable_name``)
      - ``call_function`` (requires ``function`` such as
        ``KismetSystemLibrary:PrintString``,
        ``/Script/Engine.KismetSystemLibrary:PrintString``, or a bare
        function name on the same Blueprint's class)
      - ``branch`` / ``if_then_else``
      - ``dynamic_cast`` (requires ``target_class``)
      - ``self``
      - ``format_text``
      - ``execution_sequence`` (alias ``sequence``)
      - ``knot``
      - ``make_array``
      - ``custom_event`` (optional ``event_name``)
      - ``event`` (override-style; requires ``event_name`` like
        ``ReceiveBeginPlay`` / ``ReceiveTick``)

    Optional per-node fields:

      - ``name`` to give the node a stable FName for follow-up wiring.
      - ``position`` (``[x, y]`` array) or ``pos_x`` / ``pos_y``.
      - ``pin_defaults`` (dict mapping pin name to default value).

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        nodes: List of node specs (preferred form for batched creation).
        node: A single node spec (alternative to ``nodes``).
        graph: Optional graph name; defaults to the first event graph.
        op: Defaults to ``add``. Future ops can extend this.
        compile: If true, compile after the batch lands.
        save: If true, save after the batch lands.

    Returns:
        Dict with ``nodes`` (created entries) and ``failures`` (errors
        per failed entry, with the original index).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"blueprint": blueprint}
    if op is not None:
        params["op"] = op
    if graph is not None:
        params["graph"] = graph
    if nodes is not None:
        params["nodes"] = nodes
    if node is not None:
        params["node"] = node
    if compile is not None:
        params["compile"] = compile
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("bp_nodes", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_nodes error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_wire(
    blueprint: str,
    connections: Optional[List[Dict[str, Any]]] = None,
    source_node: Optional[str] = None,
    source_pin: Optional[str] = None,
    dest_node: Optional[str] = None,
    dest_pin: Optional[str] = None,
    graph: Optional[str] = None,
    op: Optional[str] = None,
    disconnect: Optional[bool] = None,
    compile: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Connect or disconnect named pins between named nodes in a Blueprint
    graph.

    Pin direction is validated (the source pin must be an output, the
    destination pin must be an input) and pin compatibility runs through
    the K2 schema's ``CanCreateConnection`` helper before we make the
    link, so incompatible categories (e.g. wiring a String into an int
    pin) fail with the engine's own error text instead of silently doing
    the wrong thing.

    Compile is NOT automatic. Run ``compile_blueprint`` once at the end
    of your authoring batch, or pass ``compile=True`` here.

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        connections: List of edge specs. Each entry must include
            ``source_node`` / ``source_pin`` / ``dest_node`` / ``dest_pin``.
            An optional ``disconnect=true`` per entry breaks an existing
            wire instead of making a new one.
        source_node, source_pin, dest_node, dest_pin: Inline single-edge
            shortcut, equivalent to a one-element ``connections`` list.
        graph: Optional graph name; defaults to the first event graph.
        op: ``connect`` (default) or ``disconnect`` (shortcut for setting
            ``disconnect=true`` on every entry).
        disconnect: Per-call shortcut equivalent to ``op="disconnect"``.
        compile, save: If true, compile / save after wiring lands.

    Returns:
        Dict with ``connections`` (the edges actually applied), and
        ``failures`` (per-edge error reasons such as missing pins or
        type incompatibility).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"blueprint": blueprint}
    if op is not None:
        params["op"] = op
    elif disconnect is True:
        params["op"] = "disconnect"
    if graph is not None:
        params["graph"] = graph
    if connections is not None:
        params["connections"] = connections
    if source_node is not None:
        params["source_node"] = source_node
    if source_pin is not None:
        params["source_pin"] = source_pin
    if dest_node is not None:
        params["dest_node"] = dest_node
    if dest_pin is not None:
        params["dest_pin"] = dest_pin
    if compile is not None:
        params["compile"] = compile
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("bp_wire", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_wire error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_commit(
    blueprint: str,
    mark_structurally: Optional[bool] = None,
    compile: Optional[bool] = None,
    save: Optional[bool] = None,
    force_save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Commit pending edits on a Blueprint: mark structurally modified,
    compile, and save in one call.

    Convenience wrapper for the standard "I am done editing this
    Blueprint" cycle. Most ``bp_*`` mutating tools already compile and
    save individually, but a designer chaining
    ``bp_nodes`` -> ``bp_wire`` -> ``bp_commit`` (with ``compile=False``
    and ``save=False`` on the intermediate calls) gets one consolidated
    outcome with the compiler's error and warning lists separated for
    downstream consumption.

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        mark_structurally: If True (default), call
            ``MarkBlueprintAsStructurallyModified`` so SCS / function
            signature changes propagate. If False, run the lighter
            ``MarkBlueprintAsModified`` instead.
        compile: Run the Kismet compiler. Defaults True.
        save: Save the asset to disk after compile. Defaults True.
            Skipped automatically when compile reports errors so we do
            not pin a broken Blueprint to disk.
        force_save: Override the no-save-on-error guard. Defaults False.

    Returns:
        Dict with ``compiled``, ``compile_success``, ``saved``,
        ``error_count``, ``warning_count``, plus ``errors`` /
        ``warnings`` / ``infos`` string arrays carrying the compiler's
        own messages. ``success`` is True iff compile succeeded (or
        compile was skipped).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"blueprint": blueprint}
    if mark_structurally is not None:
        params["mark_structurally"] = mark_structurally
    if compile is not None:
        params["compile"] = compile
    if save is not None:
        params["save"] = save
    if force_save is not None:
        params["force_save"] = force_save

    try:
        response = unreal.send_command("bp_commit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_commit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_function_create(
    blueprint: str,
    function_name: str,
    inputs: Optional[List[Dict[str, Any]]] = None,
    outputs: Optional[List[Dict[str, Any]]] = None,
    pure: Optional[bool] = None,
    category: Optional[str] = None,
    keywords: Optional[str] = None,
    tooltip: Optional[str] = None,
    call_in_editor: Optional[bool] = None,
    compile: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Create a new Blueprint function with a typed signature in one call.

    Wraps `FBlueprintEditorUtils::CreateNewGraph` plus the
    FunctionEntry / FunctionResult pin authoring so a designer does not
    need to round-trip through ``create_function`` followed by N
    ``add_function_input`` / ``add_function_output`` calls.

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        function_name: FName for the new graph. Must be a valid C++
            identifier (alpha or underscore start, alphanumeric or
            underscore body).
        inputs: Optional list of ``{name, type}`` entries placed on the
            FunctionEntry node. Each entry may also include
            ``is_array``, ``is_reference``, and ``default``.
        outputs: Optional list of ``{name, type}`` entries placed on the
            FunctionResult node. Same extras as ``inputs``.
        pure: When True marks the function as Pure
            (``FUNC_BlueprintPure`` on the FunctionEntry's extra flags).
        category: Sets the function's Category metadata.
        keywords: Sets the function's Keywords metadata.
        tooltip: Sets the function's tooltip metadata.
        call_in_editor: When True, sets the FunctionEntry's
            ``bCallInEditor`` flag.
        compile: Compile the Blueprint after the function is laid down.
            Defaults True.
        save: Save the asset to disk after compile. Defaults True.

    Type tokens accepted (mirrors ``bp_variable``):
        - Scalars: bool, int, int64, byte, float, double, string, name, text.
        - Built-in structs: vector, vector2d, rotator, transform, color,
          linear_color.
        - Object refs: full ``/Script/Module.ClassName`` paths.
        - Blueprint class refs: ``/Game/...`` paths (auto-suffixed ``_C``).
        - Struct paths: ``struct:/Script/...`` or ``struct:/Game/...``.

    Returns:
        Dict with ``function_name``, ``graph_name`` (the engine
        auto-suffixes when a collision sneaks in, though this tool
        rejects exact name collisions up front), ``entry_node``,
        ``result_node`` (when outputs were declared), plus
        ``inputs_added`` / ``outputs_added`` arrays describing each
        requested pin.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "blueprint": blueprint,
        "function_name": function_name,
    }
    if inputs is not None:
        params["inputs"] = inputs
    if outputs is not None:
        params["outputs"] = outputs
    if pure is not None:
        params["pure"] = pure
    if category is not None:
        params["category"] = category
    if keywords is not None:
        params["keywords"] = keywords
    if tooltip is not None:
        params["tooltip"] = tooltip
    if call_in_editor is not None:
        params["call_in_editor"] = call_in_editor
    if compile is not None:
        params["compile"] = compile
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("bp_function_create", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_function_create error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def niagara_inspect(
    system: str,
    include_event_handlers: Optional[bool] = None,
    include_simulation_stages: Optional[bool] = None,
    include_renderers: Optional[bool] = None,
    include_parameters: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Read-only structured dump of a Niagara System asset.

    Pairs with ``material_inspect`` for the VFX side: lists emitters,
    per-emitter scripts grouped by execution stage (system / emitter /
    particle spawn / particle update / event handler / simulation stage
    / GPU compute), the user-exposed parameter store entries, the
    renderer properties chain, and a few sim-target / determinism flags.

    Args:
        system: Short asset name or full ``/Game/...`` Niagara System
            path.
        include_event_handlers: Default True.
        include_simulation_stages: Default True.
        include_renderers: Default True.
        include_parameters: Default True.

    Returns:
        Dict with ``name`` / ``path`` / ``class``, the system-level
        spawn / update script paths, an ``emitters`` array (each with
        ``name``, ``enabled``, ``sim_target`` (cpu / gpu),
        ``local_space``, ``determinism``, a ``scripts`` list grouped by
        execution stage, plus ``event_handlers``, ``simulation_stages``,
        and ``renderers`` arrays gated by the corresponding include
        flags), and a ``parameters`` array with each user-exposed
        parameter's name, type token, type path, and kind
        (primitive / data_interface / object).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"system": system}
    if include_event_handlers is not None:
        params["include_event_handlers"] = include_event_handlers
    if include_simulation_stages is not None:
        params["include_simulation_stages"] = include_simulation_stages
    if include_renderers is not None:
        params["include_renderers"] = include_renderers
    if include_parameters is not None:
        params["include_parameters"] = include_parameters

    try:
        response = unreal.send_command("niagara_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"niagara_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def material_inspect(material: str) -> Dict[str, Any]:
    """
    Read-only counterpart to ``material_edit``.

    Resolves the asset path against UEditorAssetLibrary and returns a
    structured dump of either a UMaterial or a UMaterialInstance. Useful
    when we need to diagnose a material's parameter set, expression list,
    or which expression drives each material attribute (BaseColor /
    Metallic / Roughness / Normal / EmissiveColor etc.) before editing
    it.

    UMaterial response includes: name + path + class, blend mode, two-
    sided / translucent flags, expression list (each with its FName,
    class, position, and parameter name when relevant), parameter list
    grouped by scalar / vector / texture / static_switch, per-attribute
    connected output expression (with output pin name), and the full
    used-texture list.

    UMaterialInstance response includes: parent material path, the
    parent material's full parameter list, and the instance's own
    overrides for scalar / vector / texture parameters.

    Args:
        material: Asset path of a UMaterial or UMaterialInstance.

    Returns:
        Dict matching the C++ handler's shape; see the header for the
        exact field set.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    try:
        response = unreal.send_command("material_inspect", {"material": material})
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"material_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def search_assets(
    class_filter: Optional[Union[str, List[str]]] = None,
    class_pattern: Optional[str] = None,
    include_subclasses: bool = False,
    path: Optional[Union[str, List[str]]] = None,
    recursive_paths: bool = True,
    name_pattern: Optional[str] = None,
    tag: Optional[Union[Dict[str, Any], List[Dict[str, Any]]]] = None,
    limit: int = 256,
    include_disk_size: bool = False,
) -> Dict[str, Any]:
    """
    Content-Browser-style asset search backed by ``IAssetRegistry``.

    Read-only. Returns each matching asset's path / name / class / package /
    package_path. With ``include_disk_size=True`` each row also carries the
    package's on-disk byte size pulled through
    ``IAssetRegistry::TryGetAssetPackageData``.

    Args:
        class_filter: Single class token or list of tokens. Each token can
            be a short name (``StaticMesh``), a full ``/Script/Module.Class``
            path, or a ``/Game/...`` Blueprint asset path (auto-suffixed
            with ``_C``). Combined with `class_pattern` for substring
            post-filtering.
        class_pattern: Case-insensitive substring matched against the
            asset's short class name after the ``FARFilter`` pass.
        include_subclasses: When true, sets ``FARFilter::bRecursiveClasses``
            so subclasses of the supplied class tokens are included.
        path: Single content-root prefix or list of them
            (``"/Game/Crafting"``).
        recursive_paths: When true (default), subfolders under each prefix
            are searched.
        name_pattern: Case-insensitive substring matched against the
            asset's short name.
        tag: Either a single ``{"name": "X", "value": "Y"}`` dict or a list
            of such dicts. ``value`` is optional. Maps onto
            ``FARFilter::TagsAndValues``.
        limit: Max rows returned. Default 256, hard-capped at 50000.
        include_disk_size: Adds a ``disk_size`` field per row.

    Returns:
        ``{"success": True, "assets": [...], "count": N, "matched_total":
        M, "limit_hit": bool}``. ``matched_total`` counts every row that
        passed the post-filter, even rows trimmed by the limit.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if isinstance(class_filter, list):
        params["class_list"] = class_filter
    elif isinstance(class_filter, str) and class_filter:
        params["class"] = class_filter
    if class_pattern:
        params["class_pattern"] = class_pattern
    if include_subclasses:
        params["include_subclasses"] = True
    if isinstance(path, list):
        params["path_list"] = path
    elif isinstance(path, str) and path:
        params["path"] = path
    if recursive_paths is False:
        # Server defaults to true; only forward when caller wants false.
        params["recursive_paths"] = False
    if name_pattern:
        params["name_pattern"] = name_pattern
    if tag is not None:
        params["tag"] = tag
    if isinstance(limit, int) and limit > 0:
        params["limit"] = limit
    if include_disk_size:
        params["include_disk_size"] = True

    try:
        response = unreal.send_command("search_assets", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"search_assets error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def asset_references(
    asset: str,
    direction: str = "hard_referencers",
    depth: int = 1,
    class_filter: Optional[str] = None,
    limit: int = 1024,
) -> Dict[str, Any]:
    """
    Read-only dependency-graph dump for a single asset.

    Walks ``IAssetRegistry::GetReferencers`` / ``GetDependencies`` for the
    asset's package and returns every package the walk reaches. Useful for
    answering "what would break if we delete or rename this asset?" before
    we actually delete or rename it.

    Args:
        asset: Asset path (``/Game/Foo/MyMaterial`` or
            ``/Game/Foo/MyMaterial.MyMaterial``). Internally we strip the
            asset-name suffix because the AR dependency walk is keyed by
            package name.
        direction: One of:
            - ``hard_referencers`` (default) — packages that hard-import
              the seed asset.
            - ``soft_referencers`` — packages that soft-reference it.
            - ``hard_dependencies`` — packages the seed hard-imports.
            - ``soft_dependencies`` — packages the seed soft-references.
            - ``all_referencers`` / ``all_dependencies`` — every package
              dep / ref regardless of Hard / Soft.
        depth: Transitive walk depth. Default 1 (immediate neighbours
            only). Capped at 6.
        class_filter: Optional class token (short name like ``Material``,
            full ``/Script/Module.ClassName``, or ``/Game/...`` Blueprint
            asset path). Drops rows whose asset class does not match.
        limit: Max rows returned. Default 1024, hard-capped at 50000.

    Returns:
        ``{"success": True, "assets": [...], "count": N, "matched_total":
        M, "limit_hit": bool, "depth_reached": K}`` plus seed_asset,
        seed_package, direction, and depth_requested fields for context.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "asset": asset,
        "direction": direction,
    }
    if isinstance(depth, int) and depth > 0:
        params["depth"] = depth
    if isinstance(limit, int) and limit > 0:
        params["limit"] = limit
    if class_filter:
        params["class_filter"] = class_filter

    try:
        response = unreal.send_command("asset_references", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"asset_references error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def bp_export(
    blueprint: str,
    include_events: Optional[bool] = None,
    include_functions: Optional[bool] = None,
    include_macros: Optional[bool] = None,
    include_components: Optional[bool] = None,
    include_variables: Optional[bool] = None,
    include_interfaces: Optional[bool] = None,
    include_defaults: Optional[bool] = None,
    max_pins_per_node: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Read-only canonical Blueprint-to-JSON snapshot.

    Returns a single GraphSpec-style document covering every event /
    function / macro / interface graph as a node list (with pin defaults)
    plus a flat edge list, every SCS component with class + relative
    transform + an optional flat property dump, every variable with its
    type + default, the parent class, and the implemented-interface list.
    Diff-able and useful for verifying that an MCP-driven authoring
    session left a Blueprint in the expected state.

    Sits next to ``bp_brief`` (one-page orientation), ``bp_inspect``
    (targeted queries), and ``bp_graph`` (graph traversal). Where
    ``read_blueprint_content`` returns a shallow event-graph-only
    summary, ``bp_export`` walks every graph kind and emits the same
    pin / edge fields ``bp_graph`` does.

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint path.
        include_events: Include event-graph (UbergraphPages) graphs.
            Default True.
        include_functions: Include function graphs. Default True.
        include_macros: Include macro graphs. Default True.
        include_components: Include the SCS component dump. Default True.
        include_variables: Include the Blueprint's NewVariables list.
            Default True.
        include_interfaces: Include implemented interfaces and their
            override graphs. Default True.
        include_defaults: Include each component's flat property dump
            through ``FProperty::ExportText``. Default True.
        max_pins_per_node: Per-node pin cap. Default 64. Each node will
            carry ``pins_truncated: True`` when the cap is hit.

    Returns:
        Dict with ``name``, ``path``, ``blueprint_class``,
        ``parent_class``, ``parent_class_path``, ``blueprint_type``,
        ``variables`` + ``variable_count``, ``components`` +
        ``component_count``, ``interfaces`` + ``interface_count``, plus a
        ``graphs`` array (each graph carries ``name``, ``kind``,
        ``graph_class``, ``node_count``, ``nodes`` (per-node
        ``node_name`` / ``class`` / ``title`` / ``position_x`` /
        ``position_y`` / ``guid`` / ``pins`` list / optional event /
        custom-event names) and ``edges`` (per-edge ``source_node`` /
        ``source_pin`` / ``target_node`` / ``target_pin`` / ``is_exec``)).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"blueprint": blueprint}
    if include_events is not None:
        params["include_events"] = include_events
    if include_functions is not None:
        params["include_functions"] = include_functions
    if include_macros is not None:
        params["include_macros"] = include_macros
    if include_components is not None:
        params["include_components"] = include_components
    if include_variables is not None:
        params["include_variables"] = include_variables
    if include_interfaces is not None:
        params["include_interfaces"] = include_interfaces
    if include_defaults is not None:
        params["include_defaults"] = include_defaults
    if max_pins_per_node is not None:
        params["max_pins_per_node"] = max_pins_per_node

    try:
        response = unreal.send_command("bp_export", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"bp_export error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def behavior_tree(
    tree: str,
    op: Optional[str] = None,
    include_blackboard: Optional[bool] = None,
    include_decorators: Optional[bool] = None,
    include_services: Optional[bool] = None,
    max_depth: Optional[int] = None,
    blackboard: Optional[str] = None,
    composite_class: Optional[str] = None,
    overwrite: Optional[bool] = None,
    replace: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Multi-op tool over a UBehaviorTree asset.

    Operations (selected through ``op``):
        - ``inspect`` (default): structured read-only dump of an
          existing tree (composite root + recursive children +
          decorators per child + services per composite + linked
          Blackboard key list).
        - ``create_behavior_tree``: create a new UBehaviorTree at a
          ``/Game/...`` package path with an optional linked
          Blackboard.
        - ``add_root_composite``: assign a Selector / Sequence /
          SimpleParallel as the tree's RootNode. Used right after
          ``create_behavior_tree`` to land a usable empty tree.

    Edit-slice future work (append child task / composite, insert
    decorator, append service, blackboard key edits) stays on the
    backlog.

    Args:
        tree: Short asset name or full ``/Game/...`` Behavior Tree
            path. For ``create_behavior_tree`` this is the target
            package path; for ``inspect`` / ``add_root_composite`` it
            is the existing asset path.
        op: One of ``inspect`` (default), ``create_behavior_tree``,
            or ``add_root_composite``.
        include_blackboard / include_decorators / include_services /
            max_depth: ``inspect``-only flags. See the read-only slice
            for details.
        blackboard: ``create_behavior_tree``-only optional
            ``/Game/...`` UBlackboardData path to link as the new
            tree's BlackboardAsset.
        composite_class: ``add_root_composite``-only token. One of
            ``selector`` / ``sequence`` / ``simple_parallel``
            (case-insensitive); a full UClass path is also accepted
            for any UBTCompositeNode subclass.
        overwrite: ``create_behavior_tree``-only flag. Replace an
            existing asset at the path. Default False.
        replace: ``add_root_composite``-only flag. Replace an existing
            RootNode. Default False; the call errors out if a
            RootNode is already present and ``replace=False``.
        save: Save the asset after the edit. Default True for both
            edit ops.

    Returns:
        For ``inspect`` see the read-only slice's contract. For the
        edit ops, a dict with ``operation``, ``name`` / ``path`` /
        ``class``, the input echo (``composite_class`` /
        ``blackboard_path`` etc.), and the ``saved`` flag.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"tree": tree}
    if op is not None:
        params["op"] = op
    if include_blackboard is not None:
        params["include_blackboard"] = include_blackboard
    if include_decorators is not None:
        params["include_decorators"] = include_decorators
    if include_services is not None:
        params["include_services"] = include_services
    if max_depth is not None:
        params["max_depth"] = max_depth
    if blackboard is not None:
        params["blackboard"] = blackboard
    if composite_class is not None:
        params["composite_class"] = composite_class
    if overwrite is not None:
        params["overwrite"] = overwrite
    if replace is not None:
        params["replace"] = replace
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("behavior_tree", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"behavior_tree error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def gas_edit(
    asset: Optional[str] = None,
    op: Optional[str] = None,
    path: Optional[str] = None,
    parent_class: Optional[str] = None,
    duration_policy: Optional[str] = None,
    duration_magnitude: Optional[float] = None,
    tags: Optional[Dict[str, Any]] = None,
    overwrite: Optional[bool] = None,
    compile: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Multi-op tool over Gameplay Ability System assets.

    Operations (selected through ``op``):
        - ``inspect`` (default): structured read-only dump.
          UGameplayAbility (or a Blueprint with a UGameplayAbility CDO)
          returns ability tags, cancel / block / activation owned /
          required / blocked tags, source / target required / blocked
          tags, cost + cooldown gameplay-effect class paths, and any
          AbilityTriggers. UGameplayEffect (or a Blueprint with a
          UGameplayEffect CDO) returns DurationPolicy, DurationMagnitude
          / MaxDurationMagnitude when Has-Duration, the modifier list,
          executions list, GameplayCues, and the cached asset / granted
          / blocked-ability tag containers through the public accessors
          that the GE component model migrated to in 5.3+. UAttributeSet
          (or a Blueprint with a UAttributeSet CDO) walks the CDO's
          FProperty list filtering on
          ``FGameplayAttribute::IsSupportedProperty``.
        - ``create_gameplay_ability``: NewObject's a UBlueprint at a
          ``/Game/...`` path with a UGameplayAbility-derived parent
          class (default ``/Script/GameplayAbilities.GameplayAbility``).
          Compiles and saves by default.
        - ``create_gameplay_effect``: NewObject's a UBlueprint at a
          ``/Game/...`` path with a UGameplayEffect-derived parent
          class (default ``/Script/GameplayAbilities.GameplayEffect``).
          Optional ``duration_policy`` (``instant`` / ``has_duration`` /
          ``infinite``) plus an optional ``duration_magnitude`` (literal
          float on a HasDuration effect's ScalableFloat magnitude) write
          through the CDO before the first compile.
        - ``set_gameplay_tags``: tag-container mutation on either asset
          shape. UGameplayAbility writes through the reflected
          ``AbilityTags`` / ``CancelAbilitiesWithTag`` /
          ``BlockAbilitiesWithTag`` / ``ActivationOwnedTags`` /
          ``ActivationRequiredTags`` / ``ActivationBlockedTags`` /
          ``SourceRequiredTags`` / ``SourceBlockedTags`` /
          ``TargetRequiredTags`` / ``TargetBlockedTags`` UPROPERTY
          fields. UGameplayEffect routes through
          ``FindOrAddComponent<UAssetTagsGameplayEffectComponent>`` /
          ``UTargetTagsGameplayEffectComponent`` /
          ``UBlockAbilityTagsGameplayEffectComponent`` and calls each
          component's ``SetAndApplyAssetTagChanges`` /
          ``SetAndApplyTargetTagChanges`` /
          ``SetAndApplyBlockedAbilityTagChanges`` mutator so the cached
          tag-container snapshot on the GE refreshes.

    Heavier ops (modifier add / remove, cost / cooldown rebind,
    attribute default override, GameplayCue authoring) remain on the
    backlog.

    Args:
        asset: Required for ``inspect`` / ``set_gameplay_tags``. Short
            asset name or full ``/Game/...`` path.
        op: One of ``inspect`` (default), ``create_gameplay_ability``,
            ``create_gameplay_effect``, or ``set_gameplay_tags``.
        path: Required for the create ops. Target ``/Game/...`` package
            path for the new Blueprint.
        parent_class: Optional override for the create ops. Accepts a
            full ``/Script/Module.ClassName`` path, a ``/Game/...``
            Blueprint class path (auto-suffixed with ``_C``), or a short
            class name probed against in-memory classes plus a
            ``/Script/GameplayAbilities.<Name>`` fallback.
        duration_policy: ``create_gameplay_effect`` only. One of
            ``instant`` / ``has_duration`` / ``infinite``.
        duration_magnitude: ``create_gameplay_effect`` only. Literal
            float, applied as the ScalableFloat magnitude on a
            HasDuration effect.
        tags: ``set_gameplay_tags`` only. Dict whose keys name a tag
            container on the asset (e.g. ``ability_tags``,
            ``cancel_abilities_with_tag``, ``asset_tags``,
            ``granted_tags``, ``blocked_ability_tags``) and whose
            values are arrays of fully-qualified tag strings.
        overwrite: Reuse an existing Blueprint at the create-op path
            instead of erroring. Default False.
        compile: Compile the Blueprint after the edit. Default True.
        save: Save the asset after the edit. Default True.

    Returns:
        For ``inspect`` see the read-only slice's contract. For
        ``create_*`` returns ``operation`` + ``name`` + ``path`` +
        ``class`` + ``parent_class`` + ``parent_class_short`` +
        ``compiled`` + ``saved`` (and ``duration_policy_written`` /
        ``duration_magnitude_written`` for the effect path). For
        ``set_gameplay_tags`` returns ``operation`` + ``name`` +
        ``path`` + ``resolved_class`` + ``applied`` + ``skipped`` +
        ``compiled`` + ``saved``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if asset is not None:
        params["asset"] = asset
    if op is not None:
        params["op"] = op
    if path is not None:
        params["path"] = path
    if parent_class is not None:
        params["parent_class"] = parent_class
    if duration_policy is not None:
        params["duration_policy"] = duration_policy
    if duration_magnitude is not None:
        params["duration_magnitude"] = duration_magnitude
    if tags is not None:
        params["tags"] = tags
    if overwrite is not None:
        params["overwrite"] = overwrite
    if compile is not None:
        params["compile"] = compile
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("gas_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"gas_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def landscape_inspect(
    include_components: Optional[bool] = None,
    include_heightmaps: Optional[bool] = None,
    include_weightmaps: Optional[bool] = None,
    name_pattern: Optional[str] = None,
    level_filter: Optional[str] = None,
) -> Dict[str, Any]:
    """
    Read-only structured dump of every ALandscape actor in the editor world.

    For each ALandscape: name, label, transform, GUID, component grid
    (ComponentSizeQuads / SubsectionSizeQuads / NumSubsections), per-
    proxy material driver and hole-material override, world-space proxy
    bounds (min / max / size), the registered layer list (each entry
    with layer_name, layer_info_object_path, phys_material, blend_method
    enum byte, is_no_blend, is_visibility_layer), the heightmap and
    weightmap texture lists deduplicated across components, and an
    optional per-component records array. Pairs with ``foliage_inspect``
    for terrain reasoning.

    Args:
        include_components: Emit a per-LandscapeComponent record (name,
            section_base, weightmap counts, heightmap path). Default
            False because a single landscape can have hundreds of
            components.
        include_heightmaps: Emit the deduplicated heightmap-texture
            package list per landscape. Default True.
        include_weightmaps: Emit the deduplicated weightmap-texture
            package list per landscape. Default False.
        name_pattern: Case-insensitive substring filter against the
            actor's name and outliner label.
        level_filter: Case-insensitive substring filter on the owning
            ULevel name (the World package name for sublevels).

    Returns:
        Dict with ``level_name`` / ``level_path`` and a ``landscapes``
        array. Each landscape entry carries ``name``, ``label``,
        ``class``, ``class_path``, ``level``, ``location``, ``rotation``,
        ``scale``, ``landscape_guid``, ``component_size_quads``,
        ``subsection_size_quads``, ``num_subsections``,
        ``component_count``, ``streaming_distance_multiplier``,
        ``landscape_material``, optional ``landscape_hole_material``,
        ``proxy_bounds_min``, ``proxy_bounds_max``, ``proxy_bounds_size``,
        ``xy_extent_min``, ``xy_extent_max``, ``xy_extent_size``,
        ``layers`` plus ``layer_count``, ``heightmap_texture_count``,
        ``weightmap_texture_count``, plus the optional ``components`` /
        ``heightmap_textures`` / ``weightmap_textures`` arrays.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if include_components is not None:
        params["include_components"] = include_components
    if include_heightmaps is not None:
        params["include_heightmaps"] = include_heightmaps
    if include_weightmaps is not None:
        params["include_weightmaps"] = include_weightmaps
    if name_pattern is not None:
        params["name_pattern"] = name_pattern
    if level_filter is not None:
        params["level_filter"] = level_filter

    try:
        response = unreal.send_command("landscape_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"landscape_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def foliage_inspect(
    name_pattern: Optional[str] = None,
    level_filter: Optional[str] = None,
    sample_locations: Optional[int] = None,
    sample_seed: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Read-only structured dump of every AInstancedFoliageActor in the editor world.

    For each IFA: actor name, label, transform, and a foliage_types
    array. Each foliage_type carries the type asset path, source mesh
    or actor class path, density, density adjustment factor, radius,
    per-axis scale interval, instance count, and (in editor) the
    approximated bounds of all its instances. Pairs with
    ``landscape_inspect`` for terrain reasoning.

    Args:
        name_pattern: Case-insensitive substring filter against the
            IFA's actor name and outliner label.
        level_filter: Case-insensitive substring filter on the owning
            ULevel name.
        sample_locations: Include up to N per-foliage-type instance
            world-space locations. Default 0 (omit). Capped at 1024 to
            keep responses bounded.
        sample_seed: Seed for the sampling RNG when
            ``sample_locations`` > 0. Default 0 (deterministic).

    Returns:
        Dict with ``level_name`` / ``level_path`` and a
        ``foliage_actors`` array. Each entry carries ``name``,
        ``label``, ``class``, ``class_path``, ``level``, ``location``,
        ``foliage_type_count``, ``total_instance_count``, and a
        ``foliage_types`` list with per-type ``foliage_type_name`` /
        ``foliage_type_path`` / ``foliage_type_class`` / ``source_kind``
        (``static_mesh`` / ``actor`` / ``unknown``) /
        ``source_path`` / ``density`` / ``density_adjustment_factor`` /
        ``radius`` / ``scale_x_min`` / ``scale_x_max`` / ``scale_y_min``
        / ``scale_y_max`` / ``scale_z_min`` / ``scale_z_max`` /
        ``instance_count`` / ``placed_instance_count``, plus optional
        ``approximated_bounds_min`` / ``approximated_bounds_max`` /
        ``approximated_bounds_size`` and ``sample_locations`` (each
        sample is ``{index, location: [x, y, z]}``).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if name_pattern is not None:
        params["name_pattern"] = name_pattern
    if level_filter is not None:
        params["level_filter"] = level_filter
    if sample_locations is not None:
        params["sample_locations"] = sample_locations
    if sample_seed is not None:
        params["sample_seed"] = sample_seed

    try:
        response = unreal.send_command("foliage_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"foliage_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def sequencer_edit(
    sequence: str,
    op: Optional[str] = None,
    include_tracks: Optional[bool] = None,
    include_camera_cut_track: Optional[bool] = None,
    include_sections: Optional[bool] = None,
    include_possessables: Optional[bool] = None,
    include_spawnables: Optional[bool] = None,
    max_sections_per_track: Optional[int] = None,
    actor: Optional[str] = None,
    binding_name: Optional[str] = None,
    overwrite: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Multi-op tool over a ULevelSequence asset.

    Operations (selected through ``op``):
        - ``inspect`` (default): read-only structured dump of an
          existing sequence. Returns master tracks, per-section
          start / end / duration, possessables, spawnables, plus
          playback range and tick / display frame rates.
        - ``create_level_sequence``: create a new ULevelSequence at a
          ``/Game/...`` package path with default tick / display
          rates and an empty MovieScene (through
          ULevelSequence::Initialize).
        - ``add_possessable``: bind a named editor-world actor to an
          existing ULevelSequence. Wraps UMovieScene::AddPossessable +
          UMovieSceneSequence::BindPossessableObject so the Sequencer
          UI picks the binding up the next time the asset opens.

    Edit-slice future work (track add, section add, section move,
    spawnable creation, camera-cut creation) stays on the backlog.

    Args:
        sequence: For ``inspect`` / ``add_possessable`` the existing
            Level Sequence asset path or short name. For
            ``create_level_sequence`` the target ``/Game/...`` package
            path.
        op: One of ``inspect`` (default), ``create_level_sequence``,
            or ``add_possessable``.
        include_tracks / include_camera_cut_track / include_sections /
            include_possessables / include_spawnables /
            max_sections_per_track: ``inspect``-only flags.
        actor: ``add_possessable``-only target actor name (matched
            against GetName() first and Outliner label second).
        binding_name: ``add_possessable``-only friendly name for the
            FMovieScenePossessable. Optional; falls back to the
            actor's GetActorLabel().
        overwrite: ``create_level_sequence``-only flag. Replace an
            existing asset at the path. Default False.
        save: Save the asset after the edit. Default True for both
            edit ops.

    Returns:
        For ``inspect`` see the read-only slice's contract. For
        ``create_level_sequence`` a dict with ``operation`` /
        ``name`` / ``path`` / ``class`` / ``tick_resolution`` /
        ``display_rate`` / ``saved``. For ``add_possessable`` a dict
        with ``operation`` / ``sequence`` / ``guid`` /
        ``binding_name`` / ``actor`` / ``actor_label`` /
        ``actor_class`` / ``actor_class_path`` / ``saved``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"sequence": sequence}
    if op is not None:
        params["op"] = op
    if include_tracks is not None:
        params["include_tracks"] = include_tracks
    if include_camera_cut_track is not None:
        params["include_camera_cut_track"] = include_camera_cut_track
    if include_sections is not None:
        params["include_sections"] = include_sections
    if include_possessables is not None:
        params["include_possessables"] = include_possessables
    if include_spawnables is not None:
        params["include_spawnables"] = include_spawnables
    if max_sections_per_track is not None:
        params["max_sections_per_track"] = max_sections_per_track
    if actor is not None:
        params["actor"] = actor
    if binding_name is not None:
        params["binding_name"] = binding_name
    if overwrite is not None:
        params["overwrite"] = overwrite
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("sequencer_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"sequencer_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def project_context(
    op: Optional[str] = None,
    include_plugins: Optional[bool] = None,
    include_modules: Optional[bool] = None,
    include_content_roots: Optional[bool] = None,
    include_engine_plugins: Optional[bool] = None,
    content_roots_recursive_count: Optional[bool] = None,
    max_content_roots: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Read-only one-shot summary of the loaded Unreal project.

    Designer-readable handle for "what kind of project am I in" before
    chasing assets. Returns project name + uproject path, engine
    version, the .uproject's user-installed plugins, the source-module
    list, the top-level Content folders with asset counts, the current
    level path, and the GameMode + default pawn (per-level override
    plus project-wide default).

    Args:
        op: Operation discriminator. Only ``context`` (default) is
            supported in this slice.
        include_plugins: Emit ``enabled_plugins``. Default True.
        include_modules: Emit ``source_modules``. Default True.
        include_content_roots: Emit ``content_roots``. Default True.
        include_engine_plugins: Include engine-shipped plugins in the
            plugins array. Default False; we filter to project / external
            / mod / enterprise plugins so the response is bounded.
        content_roots_recursive_count: Count assets recursively under
            each top-level folder. Default True.
        max_content_roots: Cap on the content-roots emission. Default
            64.

    Returns:
        Dict with ``project_name``, ``uproject_path``, ``project_dir``,
        ``project_content_dir``, optional ``project_description`` /
        ``project_category`` / ``engine_association`` /
        ``is_enterprise_project``, ``engine_version`` /
        ``engine_compatible_version`` / ``engine_major`` /
        ``engine_minor`` / ``engine_patch`` / ``engine_changelist`` /
        ``engine_branch`` / ``engine_is_licensee``,
        ``current_level_name`` / ``current_level_path``,
        optional ``default_game_mode_class`` /
        ``default_pawn_class`` (per-level override) and
        ``default_game_mode_class_project`` /
        ``default_game_map`` / ``transition_map`` /
        ``editor_startup_map`` / ``game_instance_class``,
        ``enabled_plugins`` (with per-plugin ``name`` / ``friendly_name``
        / ``type`` / ``location`` / ``version`` / ``version_name`` /
        ``category`` / ``description`` / ``created_by`` /
        ``engine_version`` / ``can_contain_content`` / ``is_beta`` /
        ``is_experimental`` / ``base_dir``), ``source_modules`` (per
        module ``name`` / ``type`` / ``loading_phase``),
        ``content_roots`` (per root ``name`` / ``path`` /
        ``asset_count``).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if op is not None:
        params["op"] = op
    if include_plugins is not None:
        params["include_plugins"] = include_plugins
    if include_modules is not None:
        params["include_modules"] = include_modules
    if include_content_roots is not None:
        params["include_content_roots"] = include_content_roots
    if include_engine_plugins is not None:
        params["include_engine_plugins"] = include_engine_plugins
    if content_roots_recursive_count is not None:
        params["content_roots_recursive_count"] = content_roots_recursive_count
    if max_content_roots is not None:
        params["max_content_roots"] = max_content_roots

    try:
        response = unreal.send_command("project_context", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"project_context error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def animation_inspect(
    asset: str,
    op: Optional[str] = None,
    include_bones: Optional[bool] = None,
    include_sockets: Optional[bool] = None,
    include_notifies: Optional[bool] = None,
    include_sections: Optional[bool] = None,
    include_slot_tracks: Optional[bool] = None,
    include_state_machines: Optional[bool] = None,
    max_bones: Optional[int] = None,
    max_notifies: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Read-only structured dump for animation assets.

    Resolves the asset by short name or ``/Game/...`` path and branches
    by class. USkeletalMesh returns skeleton path + LOD count + bones
    + sockets. UAnimSequence returns sampling frame rate + play length
    + additive flag + key count + notifies. UAnimMontage returns
    composite sections + slot tracks + notifies. UBlendSpace (and 1D)
    returns the per-axis FBlendParameter list and the sample count.
    UAnimBlueprint returns parent class + target skeleton +
    state-machine list + variable count. The state-machine list reads
    off the cached UAnimBlueprintGeneratedClass so it requires the BP
    to have compiled at least once.

    Args:
        asset: Short asset name or full ``/Game/...`` asset path.
        op: Operation discriminator. Only ``inspect`` (default) is
            supported.
        include_bones: Emit per-bone records on USkeletalMesh.
            Default True.
        include_sockets: Emit the socket list on USkeletalMesh.
            Default True.
        include_notifies: Emit the notify list on UAnimSequence /
            UAnimMontage. Default True.
        include_sections: Emit the composite-section list on
            UAnimMontage. Default True.
        include_slot_tracks: Emit the slot-track list on
            UAnimMontage. Default True.
        include_state_machines: Emit the state-machine list on
            UAnimBlueprint. Default True.
        max_bones: Cap on bone emission. Default 4096.
        max_notifies: Cap on notify emission. Default 1024.

    Returns:
        Dict with ``name`` / ``path`` / ``class`` / ``class_path`` and
        a ``kind`` discriminator (``skeletal_mesh`` /
        ``anim_sequence`` / ``anim_montage`` / ``blend_space`` /
        ``anim_blueprint`` / ``unknown``). Per-kind fields follow:
        skeletal_mesh has ``skeleton_path`` / ``lod_count`` /
        ``bone_count`` / ``bones`` / ``sockets``; anim_sequence has
        ``play_length`` / ``rate_scale`` / ``sampling_frame_rate`` /
        ``sampled_key_count`` / ``additive_anim_type`` / ``notifies``;
        anim_montage has ``play_length`` / ``rate_scale`` /
        ``composite_sections`` / ``slot_tracks`` / ``notifies``;
        blend_space has ``axis_count`` / ``sample_count`` / ``axes``;
        anim_blueprint has ``parent_class`` / ``parent_class_path`` /
        ``target_skeleton_path`` / ``is_template`` / ``variable_count``
        / ``state_machines``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"asset": asset}
    if op is not None:
        params["op"] = op
    if include_bones is not None:
        params["include_bones"] = include_bones
    if include_sockets is not None:
        params["include_sockets"] = include_sockets
    if include_notifies is not None:
        params["include_notifies"] = include_notifies
    if include_sections is not None:
        params["include_sections"] = include_sections
    if include_slot_tracks is not None:
        params["include_slot_tracks"] = include_slot_tracks
    if include_state_machines is not None:
        params["include_state_machines"] = include_state_machines
    if max_bones is not None:
        params["max_bones"] = max_bones
    if max_notifies is not None:
        params["max_notifies"] = max_notifies

    try:
        response = unreal.send_command("animation_inspect", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"animation_inspect error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def cpp_source(
    cls: Optional[str] = None,
    header_path: Optional[str] = None,
    source_path: Optional[str] = None,
    op: Optional[str] = None,
    include_header: Optional[bool] = None,
    include_source: Optional[bool] = None,
    max_bytes: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Read C++ source by class path or by full file path on disk.

    Useful for verifying the C++ class behind a Blueprint or for
    asking "what does this UClass actually look like in source".
    Provide one of three inputs:

    - ``cls``: a class identifier accepting ``/Script/Module.ClassName``,
      a ``/Game/...`` Blueprint class path (auto-suffixed with ``_C``),
      or a short class name (probed against the loaded class set with
      A / U prefix variants and an ``/Script/Engine.<Name>`` fallback).
      Header + cpp paths come from
      ``FSourceCodeNavigation::FindClassHeaderPath`` /
      ``FindClassSourcePath``.
    - ``header_path``: a full disk path to a .h file. The cpp follow-on
      is inferred by replacing the extension when the sibling exists.
    - ``source_path``: a full disk path to a .cpp file. The header
      follow-on is inferred by replacing the extension when the
      sibling exists.

    Args:
        cls: Class identifier (full ``/Script/...`` path, ``/Game/...``
            Blueprint path, or short class name).
        header_path: Full disk path to a .h file.
        source_path: Full disk path to a .cpp file.
        op: Operation discriminator. Only ``read`` (default) is
            supported.
        include_header: Emit the header text. Default True.
        include_source: Emit the cpp text. Default True.
        max_bytes: Cap on each emitted file's text length. Default
            262144 (256 KiB). Sets ``header_truncated`` / ``source_truncated``
            flags when the cap fires.

    Returns:
        Dict with ``class`` / ``class_short`` / ``module`` / ``module_dir``
        when class-driven, ``header_path`` / ``header_text`` /
        ``header_text_bytes`` / ``header_truncated`` / ``header_exists``,
        ``source_path`` / ``source_text`` / ``source_text_bytes`` /
        ``source_truncated`` / ``source_exists``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    if cls is None and header_path is None and source_path is None:
        return {
            "success": False,
            "message": "One of 'cls', 'header_path', or 'source_path' is required",
        }

    params: Dict[str, Any] = {}
    if cls is not None:
        params["class"] = cls
    if header_path is not None:
        params["header_path"] = header_path
    if source_path is not None:
        params["source_path"] = source_path
    if op is not None:
        params["op"] = op
    if include_header is not None:
        params["include_header"] = include_header
    if include_source is not None:
        params["include_source"] = include_source
    if max_bytes is not None:
        params["max_bytes"] = max_bytes

    try:
        response = unreal.send_command("cpp_source", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"cpp_source error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def pie_test_scene(
    assertions: List[Dict[str, Any]],
) -> Dict[str, Any]:
    """
    Scene-state assertion harness.

    Runs a list of assertion specs against the active editor world and
    returns a per-assertion pass / fail record plus aggregate counts.
    Does not drive Play in Editor. Answers each supported assertion
    statically against the editor world. Four assertion kinds are
    supported:

        - ``actor_exists``: ``target`` is an actor name. Pass = an actor
          with that ``GetName()`` or Outliner label is present.
        - ``actor_at_location``: ``target`` is an actor name,
          ``expected`` is a ``[x, y, z]`` world-space location, and
          ``tolerance`` (default 1.0 cm) is the pass radius. Pass = the
          resolved actor's ``GetActorLocation`` is within ``tolerance``
          of ``expected``.
        - ``actor_overlapping_tag``: ``target`` is an actor name,
          ``expected`` is an FName tag string. Pass = the resolved
          actor's ``Tags`` array contains that FName.
        - ``var_equals``: ``target`` is an actor name, ``expected``
          is a ``{var, value}`` dict. Pass = the resolved actor's
          UPROPERTY (looked up by FName) ImportText-matches the
          canonicalized representation of ``value``. Works against
          transform fields, gameplay tags, FString fields, and any
          other Blueprint-exposed variable.

    Args:
        assertions: Array of assertion specs. Each entry is a dict
            ``{kind, target, expected?, tolerance?}``. Required;
            non-empty.

    Returns:
        Dict with ``total``, ``passed``, ``failed``, ``unsupported``,
        ``all_passed``, and a ``results`` array. Each result row
        carries ``index``, ``kind``, ``target``, ``passed`` flag,
        optional ``actual`` / ``expected`` / ``delta`` /
        ``tolerance`` / ``var`` / ``property_class`` for the
        relevant kinds, and ``message``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"assertions": assertions}

    try:
        response = unreal.send_command("pie_test_scene", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"pie_test_scene error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def animation_edit(
    op: str,
    asset: str,
    rate_scale: Optional[float] = None,
    additive_type: Optional[str] = None,
    ref_pose_type: Optional[str] = None,
    ref_pose_seq: Optional[str] = None,
    ref_frame_index: Optional[int] = None,
    track: Optional[str] = None,
    frame: Optional[int] = None,
    time: Optional[float] = None,
    notify_class: Optional[str] = None,
    event_name: Optional[str] = None,
    duration: Optional[float] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Targeted UAnimSequence / UAnimMontage edits (small variant).

    Multi-op tool keyed by ``op``:

        - ``set_rate_scale``: write ``RateScale`` (float) on a
          UAnimSequenceBase. Applies to both UAnimSequence and
          UAnimMontage.
        - ``set_additive``: toggle the additive shape on a
          UAnimSequence. Writes ``AdditiveAnimType`` (none /
          local_space / rotation_offset_mesh_space) plus optional
          ``ref_pose_type`` (none / ref_pose / anim_scaled /
          anim_frame), ``ref_pose_seq`` (a ``/Game/...`` UAnimSequence
          path) and ``ref_frame_index``.
        - ``add_notify``: append an FAnimNotifyEvent to a notify
          track on a UAnimSequenceBase. Auto-creates the track when
          missing. ``notify_class`` resolves to either UAnimNotify
          or UAnimNotifyState; ``duration`` is required for
          UAnimNotifyState. When ``notify_class`` is omitted we
          treat the entry as a custom-event notify (writes
          ``event_name`` only).

    Args:
        op: One of ``set_rate_scale`` / ``set_additive`` /
            ``add_notify``. Required.
        asset: Short asset name or full ``/Game/...`` path. Required.
        rate_scale: Float; required for ``set_rate_scale``.
        additive_type: Token; required for ``set_additive``.
        ref_pose_type: Optional ref-pose type token for
            ``set_additive``.
        ref_pose_seq: Optional ``/Game/...`` UAnimSequence path for
            ``set_additive``.
        ref_frame_index: Optional integer frame index for
            ``set_additive``.
        track: Notify track FName; required for ``add_notify``.
        frame: Integer frame index; one of ``frame`` / ``time`` is
            required for ``add_notify``.
        time: Float seconds; alternative to ``frame``.
        notify_class: Optional UAnimNotify or UAnimNotifyState
            class (short name or full path). When omitted the
            notify is treated as a custom-event notify.
        event_name: Optional FName for the custom-event notify.
        duration: Float seconds; required for UAnimNotifyState
            subclasses.
        save: Persist the asset on success. Default True.

    Returns:
        Op-specific dict. ``set_rate_scale`` reports the previous
        and new ``rate_scale``; ``set_additive`` reports the
        resolved enum tokens; ``add_notify`` reports
        ``track_name``, ``time``, ``notify_class``, ``notify_kind``,
        ``notify_count``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"op": op, "asset": asset}
    if rate_scale is not None:
        params["rate_scale"] = rate_scale
    if additive_type is not None:
        params["additive_type"] = additive_type
    if ref_pose_type is not None:
        params["ref_pose_type"] = ref_pose_type
    if ref_pose_seq is not None:
        params["ref_pose_seq"] = ref_pose_seq
    if ref_frame_index is not None:
        params["ref_frame_index"] = ref_frame_index
    if track is not None:
        params["track"] = track
    if frame is not None:
        params["frame"] = frame
    if time is not None:
        params["time"] = time
    if notify_class is not None:
        params["notify_class"] = notify_class
    if event_name is not None:
        params["event_name"] = event_name
    if duration is not None:
        params["duration"] = duration
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("animation_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"animation_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def foliage_edit(
    op: str,
    foliage_type: str,
    level: Optional[str] = None,
    density: Optional[float] = None,
    density_adjustment_factor: Optional[float] = None,
    radius: Optional[float] = None,
    scale_x_min: Optional[float] = None,
    scale_x_max: Optional[float] = None,
    scale_y_min: Optional[float] = None,
    scale_y_max: Optional[float] = None,
    scale_z_min: Optional[float] = None,
    scale_z_max: Optional[float] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Foliage authoring (small variant).

    Two ops on AInstancedFoliageActor / UFoliageType, keyed by ``op``:

        - ``add_foliage_type``: register a UFoliageType asset on the
          AInstancedFoliageActor for a chosen level. Resolves (or
          spawns) the IFA through ``AInstancedFoliageActor::Get`` and
          binds the type through ``AInstancedFoliageActor::AddFoliageType``.
          Reuses an existing FFoliageInfo when the type is already
          registered.
        - ``set_foliage_density``: write ``Density``,
          ``DensityAdjustmentFactor``, ``Radius``, and the per-axis
          ``ScaleX`` / ``ScaleY`` / ``ScaleZ`` FFloatInterval pairs
          on a UFoliageType asset. All fields are optional; only
          the ones present in the call are written.

    Args:
        op: One of ``add_foliage_type`` / ``set_foliage_density``.
            Required.
        foliage_type: Short asset name or ``/Game/...`` UFoliageType
            path. Required.
        level: Optional substring on owning ULevel name. Used by
            ``add_foliage_type`` to pick a sublevel; defaults to
            the persistent level when omitted.
        density / density_adjustment_factor / radius: Optional floats
            for ``set_foliage_density``.
        scale_x_min / scale_x_max / scale_y_min / scale_y_max /
        scale_z_min / scale_z_max: Optional floats for
            ``set_foliage_density``. Each axis interval requires both
            min and max to be present together.
        save: Persist on success. Default True for
            ``set_foliage_density``; False for ``add_foliage_type``
            (the IFA is a level actor, not an asset).

    Returns:
        Op-specific dict. ``add_foliage_type`` reports the IFA's
        ``actor_path``, ``level``, ``created_actor``, and
        ``type_already_bound`` flags;
        ``set_foliage_density`` reports the previous and new values
        for every field actually written.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"op": op, "foliage_type": foliage_type}
    if level is not None:
        params["level"] = level
    if density is not None:
        params["density"] = density
    if density_adjustment_factor is not None:
        params["density_adjustment_factor"] = density_adjustment_factor
    if radius is not None:
        params["radius"] = radius
    if scale_x_min is not None:
        params["scale_x_min"] = scale_x_min
    if scale_x_max is not None:
        params["scale_x_max"] = scale_x_max
    if scale_y_min is not None:
        params["scale_y_min"] = scale_y_min
    if scale_y_max is not None:
        params["scale_y_max"] = scale_y_max
    if scale_z_min is not None:
        params["scale_z_min"] = scale_z_min
    if scale_z_max is not None:
        params["scale_z_max"] = scale_z_max
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("foliage_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"foliage_edit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def performance_audit(
    frames: Optional[int] = None,
    metrics: Optional[List[str]] = None,
    include_samples: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    Read-only performance snapshot for the active editor viewport.

    Reads the live ``FStatUnitData`` ring on the editor's active
    viewport (a 200-sample circular buffer that ``stat unit`` already
    populates) plus the ``GAverageMS`` / ``GAverageFPS`` /
    ``GGameThreadTime`` / ``GRenderThreadTime`` / ``GRHIThreadTime``
    cycle-counter globals, and returns a per-metric ``avg_ms`` /
    ``peak_ms`` / ``last_ms`` triple over the last ``frames`` samples
    plus a live ``globals`` block.

    Skips the deep-dive captures (``stat startfile`` / ``stat
    stopfile``, Insights traces, FPSChart). Those stay on the backlog.

    Args:
        frames: Window size for the running averages. Default 60,
            capped at the engine's 200-sample ring in non-shipping
            builds.
        metrics: Optional list of metric tokens to filter the report
            to a subset. Allowed tokens: ``frame`` (total frame time),
            ``game`` (game-thread), ``render`` (render-thread),
            ``rhi`` (RHI thread), ``gpu`` (GPU frame), ``globals``
            (live cycle-counter snapshot), ``samples`` (the raw
            per-frame ring dump). When omitted, every metric ships.
        include_samples: Opt-in raw per-frame array dump for each
            kept metric. Default False; the summary triples are
            usually enough.

    Returns:
        Dict with ``requested_frames``, ``max_samples``,
        ``sample_count``, optional per-metric ``frame`` / ``game`` /
        ``render`` / ``rhi`` / ``gpu`` blocks (each with ``avg_ms`` +
        ``peak_ms`` + ``last_ms`` + ``counted_samples``), an optional
        ``samples`` block (when ``include_samples=true``), the live
        ``globals`` block, and a ``viewport`` echo (or ``null`` when
        no viewport client is reachable).
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {}
    if frames is not None:
        params["frames"] = frames
    if metrics is not None:
        params["metrics"] = metrics
    if include_samples is not None:
        params["include_samples"] = include_samples

    try:
        response = unreal.send_command("performance_audit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"performance_audit error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def pie_test_bp(
    blueprint: str,
    assertions: List[Dict[str, Any]],
) -> Dict[str, Any]:
    """
    Blueprint-side assertion harness (small variant).

    Runs against a Blueprint asset's CDO without needing a running PIE
    session. Sits next to ``pie_test_scene`` (which targets actors in
    the active editor world).

    Currently supports one assertion kind:

        - ``default_value_equals``: ``target`` is a UPROPERTY FName on
          the Blueprint's generated class (or its parent class).
          ``expected`` is a JSON literal that is canonicalized through
          the property's ``ImportText`` -> ``ExportText`` round-trip
          and compared against the CDO's ``ExportText`` output. Vector
          / rotator / transform / FString / gameplay tag fields all
          flow through one path because the canonicalisation happens
          on the engine side.

    The kinds that need a running PIE session (``function_returns``,
    ``event_fired``) stay on the backlog; ``pie_test_scene`` already
    covers actor-instance assertions in the editor world.

    Args:
        blueprint: Short asset name or full ``/Game/...`` Blueprint
            path.
        assertions: List of ``{kind, target, expected}`` specs.

    Returns:
        Dict with ``blueprint`` + ``blueprint_name`` +
        ``generated_class``, aggregate counts (``total`` / ``passed``
        / ``failed`` / ``unsupported`` / ``all_passed``), and a
        per-assertion ``assertions`` array. Each assertion row carries
        ``index``, ``kind``, ``target``, ``passed`` flag, and
        kind-specific fields (``var`` / ``actual`` / ``expected`` /
        ``expected_raw`` / ``property_class`` / ``expected_imported``
        for ``default_value_equals``) plus a human-readable
        ``message``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {
        "blueprint": blueprint,
        "assertions": assertions,
    }

    try:
        response = unreal.send_command("pie_test_bp", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"pie_test_bp error: {e}")
        return {"success": False, "message": str(e)}


@mcp.tool()
def metasound_edit(
    op: str,
    path: str,
    output_format: Optional[str] = None,
    sample_rate: Optional[int] = None,
    block_rate: Optional[float] = None,
    overwrite: Optional[bool] = None,
    save: Optional[bool] = None,
) -> Dict[str, Any]:
    """
    MetaSound asset authoring (small variant).

    Two ops, keyed by ``op``:

        - ``create_metasound_source``: creates a new
          ``UMetaSoundSource`` (an audio-output MetaSound) at a
          ``/Game/...`` path. Optional ``output_format`` token
          (``mono`` / ``stereo`` / ``quad`` / ``5_1`` / ``7_1``;
          default stereo) plus optional ``sample_rate`` and
          ``block_rate`` overrides land on the asset's OutputFormat /
          SampleRateOverride / BlockRateOverride before InitAsset
          wires the document.
        - ``create_metasound_patch``: creates a new
          ``UMetaSoundPatch`` (a reusable graph asset, no audio
          output) at a ``/Game/...`` path.

    Both ops route through ``UMetaSoundEditorSubsystem::GetChecked()``'s
    public ``InitAsset`` + ``RegisterGraphWithFrontend`` so the new
    asset has a fresh document plus an editor graph that opens cleanly
    in the MetaSound editor.

    The graph-authoring surface (add nodes, connect pins, set member
    defaults) stays on the BACKLOG. That surface lives behind the
    UMetaSoundBuilder API which has its own learning curve.

    Args:
        op: One of ``create_metasound_source`` (default) or
            ``create_metasound_patch``.
        path: Target ``/Game/...`` package path. Required.
        output_format: One of ``mono`` / ``stereo`` / ``quad`` /
            ``5_1`` / ``7_1``. Default stereo. ``create_source`` only.
        sample_rate: Optional integer Hz. ``create_source`` only.
            Zero keeps the device default.
        block_rate: Optional float Hz. ``create_source`` only. Zero
            keeps the device default.
        overwrite: Reuse an existing asset at the path instead of
            erroring. Default False.
        save: Save the asset after the edit. Default True.

    Returns:
        Dict with ``operation`` + ``name`` + ``path`` + ``class`` +
        the relevant create-op echo (``output_format`` /
        ``sample_rate`` / ``block_rate`` for the source path) +
        ``saved``.
    """
    unreal = get_unreal_connection()
    if not unreal:
        return {"success": False, "message": "Failed to connect to Unreal Engine"}

    params: Dict[str, Any] = {"op": op, "path": path}
    if output_format is not None:
        params["output_format"] = output_format
    if sample_rate is not None:
        params["sample_rate"] = sample_rate
    if block_rate is not None:
        params["block_rate"] = block_rate
    if overwrite is not None:
        params["overwrite"] = overwrite
    if save is not None:
        params["save"] = save

    try:
        response = unreal.send_command("metasound_edit", params)
        return response or {"success": False, "message": "No response from Unreal"}
    except Exception as e:
        logger.error(f"metasound_edit error: {e}")
        return {"success": False, "message": str(e)}


# Run the server
if __name__ == "__main__":
    logger.info("Starting Advanced MCP server with stdio transport")
    mcp.run(transport='stdio')