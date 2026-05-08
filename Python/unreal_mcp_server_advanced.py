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
from typing import AsyncIterator, Dict, Any, Optional, List
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
) -> Dict[str, Any]:
    """
    Create a new asset of the requested type.

    Mirrors the hosted Flop "asset_factory" surface. This fork supports four
    asset types so far: DataTable, Enum, Struct, and DataAsset. The Enhanced
    Input data-asset bundle is split into the dedicated bp_input tool.

    Args:
        asset_type: One of "datatable", "enum", "struct", "data_asset".
        package_path: Absolute content-browser path for the new asset, e.g.
            "/Game/Data/CraftingRecipes". A trailing ".AssetName" object
            suffix is allowed and stripped.
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
    expose_as_variable: bool = True,
    save: bool = True,
    overwrite: bool = False,
) -> Dict[str, Any]:
    """
    Edit a Widget Blueprint asset.

    Mirrors a small slice of the hosted Flop "widget_edit" tool. Two operations
    are supported in this first cut:
        - "create_widget_blueprint": create a UWidgetBlueprint at a path with
          a parent UUserWidget class and an optional root panel class.
        - "add_child_widget": construct a named widget (e.g. vertical_box,
          progress_bar, text_block, button, image) and attach it to an
          existing parent panel inside an existing Widget Blueprint.

    Args:
        operation: "create_widget_blueprint" or "add_child_widget".
        package_path: For create: the absolute content-browser path for the
            new asset, e.g. "/Game/UI/WBP_CraftingMenu".
        widget_blueprint: For add_child_widget: the absolute path to the
            existing widget blueprint, e.g. "/Game/UI/WBP_CraftingMenu".
        parent_class: For create: a UUserWidget subclass path or short name.
            Defaults to UUserWidget.
        root_panel_class: For create: a UPanelWidget subclass path or short
            name to seed the root widget. Defaults to UCanvasPanel.
        widget_type: For add_child_widget: the widget type to construct. One
            of vertical_box, horizontal_box, canvas_panel, overlay, scroll_box,
            border, size_box, spacer, progress_bar, text_block, button, image,
            or a fully qualified UWidget class path.
        widget_name: For add_child_widget: the FName for the new widget. Must
            be unique within the asset.
        parent_name: For add_child_widget: the FName of the parent panel
            inside the asset. If omitted, the asset's root panel is used.
        text: For add_child_widget: optional initial text for text_block.
        expose_as_variable: For add_child_widget: mark the new widget as a
            Blueprint variable so other graphs can bind to it. Defaults True.
        save: Save the asset after the change. Defaults True.
        overwrite: For create: overwrite an existing asset at package_path.
            Defaults False.
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
) -> Dict[str, Any]:
    """
    Small slice of material authoring: create materials and instances, or
    set scalar / vector / texture overrides on a Material Instance Constant.

    Mirrors the constrained subset of the hosted Flop "material_edit" tool.
    Three operations are supported in this first cut:

        - "create_material": create a UMaterial with an optional Constant3
          base-colour driver wired into the BaseColor input.
        - "create_material_instance_constant": create a UMaterialInstance
          Constant pointing at a parent material or instance.
        - "set_instance_parameter": override scalar / vector / texture
          parameters on a Material Instance Constant. The parameter type
          is auto-detected from the supplied value, or pinned with the
          parameter_type hint.

    Authoring expression graphs and Material Parameter Collections is
    deferred to a later pass; see BACKLOG.md.

    Args:
        operation: "create_material", "create_material_instance_constant",
            or "set_instance_parameter".
        package_path: For create operations: absolute /Game/ path for the
            new asset, e.g. "/Game/Materials/M_Default".
        base_color: For create_material: optional [r, g, b] or [r, g, b, a]
            linear colour wired into BaseColor through a Constant3Vector
            expression. Skip to leave BaseColor unwired.
        parent_material: For create_material_instance_constant: absolute
            path to the parent UMaterialInterface.
        material_instance: For set_instance_parameter: absolute path to the
            target UMaterialInstanceConstant.
        parameter_name: For set_instance_parameter: parameter FName.
        parameter_type: For set_instance_parameter: optional hint of
            "scalar", "vector" / "color", or "texture". Defaults to
            auto-detect from the value.
        value: For set_instance_parameter: the override value. Accepts a
            number (scalar), a [r, g, b, a] array or {"r":..,"g":..} dict
            (vector), or a texture asset path string (texture).
        save: Save the asset after the change. Defaults True.
        overwrite: For create operations: overwrite existing assets at
            package_path. Defaults False.

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


# Run the server
if __name__ == "__main__":
    logger.info("Starting Advanced MCP server with stdio transport")
    mcp.run(transport='stdio') 