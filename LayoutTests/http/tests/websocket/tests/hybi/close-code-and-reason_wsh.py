import struct

from pywebsocket3 import stream

def web_socket_do_extra_handshake(request):
    pass

def web_socket_transfer_data(request):
    line = request.ws_stream.receive_message()
    if line is None:
        return
    if line == '-':
        data = b''
    elif line == '--':
        data = b'X'
    else:
        code, reason = line.split(' ', 1)
        data = struct.pack('!H', int(code)) + reason.encode('utf-8')
    request.connection.write(stream.create_close_frame(data))
