import random

from prompt_toolkit import document
from pyvis.network import Network
import networkx as nx

def build_graph(edges, directed=False):
    G = nx.DiGraph() if directed else nx.Graph()
    G.add_edges_from(edges)
    return G

# def dfs_tree_and_back_edges(G, start_node):
#     visited = set()
#     dfs_tree = nx.DiGraph()
#
#     def dfs(u):
#         visited.add(u)
#         for v in G[u]:
#             if v not in visited:
#                 dfs_tree.add_edge(u, v)
#                 dfs(v, ancestors)
#             elif v in ancestors:
#                 back_edges.append((u, v))
#         ancestors.remove(u)
#
#     dfs(start_node, set())
#     return dfs_tree, back_edges

def add_line(net, start_x, start_y, end_x, end_y):
    """
    Add a line to the network by creating invisible nodes and edges between them.

    Args:
        net: pyvis Network object
        start_x, start_y: starting coordinates
        end_x, end_y: ending coordinates
        line_id: prefix for line nodes ids
    """
    # Create invisible nodes for line endpoints
    line_id = random.randint(0, 9999999999)
    net.add_node(f"{line_id}_start", x=start_x, y=start_y,
                 size=0.1,  # Very small size
                 color='rgba(255,255,255,0)',  # Transparent
                 label='',  # No label
                 font={'size': 0},
                 physics=False)  # Don't participate in physics simulation

    net.add_node(f"{line_id}_end", x=end_x, y=end_y,
                 size=0.1,
                 color='rgba(255,255,255,0)',
                 label='',
                 font={'size': 0},
                 physics=False)  # Don't participate in physics simulation

    # Add edge between nodes to create the line
    net.add_edge(f"{line_id}_start", f"{line_id}_end", color='black', width=5)


def visualize_interactive(dfs_tree, back_edges, sourced_nodes, matched_data_nodes, data_dict, subtree_sizes_dict, output_file="graph.html"):
    net = Network(height="750px", width="100%", directed=True, notebook=False)
    net.from_nx(dfs_tree)

    # Add back edges in red and dashed
    # for u, v in back_edges:
    #     net.add_edge(u, v, color="red", dashes=True, title="Back Edge")

    for node in net.nodes:
        node['size'] = 25  # Make nodes bigger
        # node['title'] = data_dict[node['id']] if node['id'] in data_dict else ''
        size = ('size: ' + subtree_sizes_dict[node['id']]) if node['id'] in subtree_sizes_dict else ''
        node['title'] = 'id: ' + str(node['id']) + '\n' + size
        node['label'] = str(node['id'])
        if (node['id'] in sourced_nodes):
            node['color'] = {'background': '#ff695e'}
        elif (node['id'] in matched_data_nodes):
            node['color'] = {'background': 'orange', 'border': 'orange'}
        node['font'] = {'size': 20, 'color': 'black'}  # Make label text bigger and black

    # Example: Add a horizontal line at y=100
    # add_line(net, 0, 0, 500, 500)
    # Example: Add a vertical line at x=0
    # add_line(net, 0, -500, 0, 500, "vertical_line")

    # Convert back_edges to a JavaScript array string
    back_edges_js = "["
    for i, (u, v) in enumerate(back_edges):
        back_edges_js += f"[{u}, {v}]"
        if i < len(back_edges) - 1:
            back_edges_js += ", "
    back_edges_js += "]"
    
    data_dict_js = "{"
    for i, (key, value) in enumerate(data_dict.items()):
        data_dict_js += f'"{key}": "{value}"'
        if i < len(data_dict) - 1:
            data_dict_js += ", "
    data_dict_js += "}"

    # Set options for the network
    net.set_options("""
        var options = {
          "nodes": {
            "shape": "dot",
            "size": 16,
            "font": {
              "size": 18
            }
          },
          "edges": {
            "arrows": {
              "to": {
                "enabled": true
              }
            },
            "smooth": {
              "type": "dynamic"
            }
          },
          "layout": {
            "hierarchical": {
              "enabled": true,
              "direction": "UD",
              "sortMethod": "hubsize",  
              "levelSeparation": 150,
              "nodeSpacing": 100
            }
          },
          "physics": {
            "enabled": false
          }
        }
        """)

    # Generate the HTML file
    net.write_html(output_file)

    # Read the generated HTML file
    with open(output_file, 'r') as f:
        html_content = f.read()

    # Add custom JavaScript code to draw back edges
    custom_js = f"""
    <script type="text/javascript">
        // Define the back edges array
        var back_edges = {back_edges_js};
        var data_dict = {data_dict_js};
        function print_text(x, y, text) {{
            var canvas = document.getElementById('my_canvas');
            if (canvas == null) {{
                canvas = document.createElement('canvas');
                canvas.id = 'my_canvas';
                document.getElementById('mynetwork').children[0].appendChild(canvas);
                canvas.style.position = 'absolute';
                canvas.style.top = '0';
                canvas.style.left = '0';
                canvas.style.pointerEvents = 'none';
                canvas.width = document.getElementById('mynetwork').clientWidth;
                canvas.height = document.getElementById('mynetwork').clientHeight;
            }}
            var ctx = canvas.getContext('2d');
            ctx.font = '12px Arial';
            ctx.fillStyle = 'black';
            pos = network.canvasToDOM({{x: x, y: y}})
            ctx.fillText(text, pos.x, pos.y);
        }}
        

        // Function to draw a line between two points
        function draw_line(a, b) {{
            // Get the canvas element
            var canvas = document.getElementById('my_canvas');
            if (canvas == null) {{
                canvas = document.createElement('canvas');
                canvas.id = 'my_canvas';
                document.getElementById('mynetwork').children[0].appendChild(canvas);
                canvas.style.position = 'absolute';
                canvas.style.top = '0';
                canvas.style.left = '0';
                canvas.style.pointerEvents = 'none';
                canvas.width = document.getElementById('mynetwork').clientWidth;
                canvas.height = document.getElementById('mynetwork').clientHeight;
            }}

            // Get the context
            var ctx = canvas.getContext('2d');

            // Set line style
            ctx.strokeStyle = 'red';
            ctx.lineWidth = 1;
            //ctx.setLineDash([5, 3]); // Dashed line

            // Draw the line
            ctx.beginPath();
            //console.log(a.x, a.y, b.x, b.y);
            a = network.canvasToDOM(a)
            b = network.canvasToDOM(b)
            //console.log(a.x, a.y, b.x, b.y);
            ctx.moveTo(a.x, a.y);
            ctx.lineTo(b.x, b.y);
            //ctx.moveTo(0, 0);
            //ctx.lineTo(500, 500);
            ctx.stroke();
        }}


        network.on('click', function(e) {{
            console.log(e);
            canvas = document.getElementById('my_canvas')
            // remove element by id
            //if (canvas != null) {{
            //    document.getElementById('my_canvas').remove();
            //}}
            // For each back edge
            for (i = 0; i < e.nodes.length; i++) {{
                var node = e.nodes[i];
                //alert(data_dict[node.toString()])
                if (data_dict[node.toString()] == undefined) continue;
                print_text(network.getPosition(node).x, network.getPosition(node).y, data_dict[node.toString()]);
            }}
            for (var i = 0; i < back_edges.length; i++) {{
                var edge = back_edges[i];
                var from_node = edge[0];
                var to_node = edge[1];
                // If from_node is not in e.nodes array, continue
                if (!e.nodes.includes(from_node)) continue;
                

                // Get positions of the nodes
                var from_pos = network.getPosition(from_node);
                var to_pos = network.getPosition(to_node);
              //  console.log(from_pos, to_pos);

                // Draw line between the positions
                draw_line(from_pos, to_pos);
            }}
        }});
    </script>
    """

    # Insert the custom JavaScript code before the closing body tag
    html_content = html_content.replace('</body>', custom_js + '</body>')

    # Write the modified HTML back to the file
    with open(output_file, 'w') as f:
        f.write(html_content)


# === Example Usage ===

lines = open('input.txt').read().splitlines()
direct_edges_inp = lines[0]
back_edges_inp = lines[1] if len(lines) > 1 else ''
sourced_nodes_inp = lines[2] if len(lines) > 2 else ''
mathed_data_nodes_inp = lines[3] if len(lines) > 3 else ''
lines = lines[4:]
nodes_data_cnt = int(lines[0]) if len(lines) > 0 else 0
data_dict = {}
for i in range(1, 1 + nodes_data_cnt):
    node_id, data = lines[i].split(' ')
    data_dict[int(node_id)] = data
lines = lines[1 + nodes_data_cnt:]
subtree_sizes_cnt = int(lines[0]) if len(lines) > 0 else 0
subtree_sizes_dict = {}
for i in range(1, 1 + subtree_sizes_cnt):
    node_id, subtree_size = lines[i].split(' ')
    subtree_sizes_dict[int(node_id)] = str(int(subtree_size) // 8)

sourced_nodes = [] if sourced_nodes_inp == '' else [int(x) for x in sourced_nodes_inp.split(',')]
matched_data_nodes = [] if mathed_data_nodes_inp == '' else [int(x) for x in mathed_data_nodes_inp.split(',')]


def parse_edges(input_str):
    edges = []
    for line in input_str.strip().split(','):
        # Split each line by '>' character
        nodes = line[1:-1].strip().split('>')
        if len(nodes) == 2:
            # Convert to integers and add as tuple
            from_node = int(nodes[0])
            to_node = int(nodes[1])
            edges.append((from_node, to_node))
    return edges

# Parse the input and build the graph
direct_edges = parse_edges(direct_edges_inp)
back_edges = parse_edges(back_edges_inp)

G = build_graph(direct_edges, directed=True)
# dfs_tree = dfs_tree_and_back_edges(G, start_node=0)
visualize_interactive(G, back_edges, sourced_nodes, matched_data_nodes, data_dict, subtree_sizes_dict)
