# simple http file relay/dropbox server
import argparse
import http.server
import os

class UploadHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        # Extract filename from the request path (e.g., /filename.txt)
        filename = os.path.basename(self.path)
        if not filename:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"Missing filename in URL path")
            return

        # Read the file data from the request body
        content_length = int(self.headers['Content-Length'])
        file_data = self.rfile.read(content_length)

        # Save the file to disk
        with open(filename, 'wb') as f:
            f.write(file_data)

        self.send_response(200)
        self.end_headers()
        self.wfile.write(f"Successfully saved {filename}\n".encode())

    def do_GET(self):
        # Extract filename from the request path (e.g., /filename.txt)
        filename = os.path.basename(self.path)
        if not filename:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b"Missing filename in URL path")
            return
        if not os.path.isfile(filename):
            self.send_response(404)
            self.end_headers()
            self.wfile.write(f"File not found: {filename}\n".encode())
            return
        # Send the file back to the client
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Disposition', f'attachment; filename="{filename}"')
        self.send_header('Content-Length', str(os.path.getsize(filename)))
        self.end_headers()
        with open(filename, 'rb') as f:
            self.wfile.write(f.read())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Simple file upload/download server")
    parser.add_argument("port", type=int, help="TCP port to listen on")
    args = parser.parse_args()
    server = http.server.HTTPServer(('0.0.0.0', args.port), UploadHandler)
    print(f"Server listening on port {args.port}...")
    server.serve_forever()
