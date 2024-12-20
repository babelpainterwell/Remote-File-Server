# Remote File Server

This project is basically a small client-server application written in C that lets us store, retrieve, and remove files on a remote server. The client program supports commands like WRITE (to send a local file up to the server), GET (to download a file from the server), and RM (to remove a file on the server’s side).

Behind the scenes, it uses TCP sockets for network communication, a simple XOR-based scheme to "encrypt" or obfuscate the file data before it’s saved, and it maintains file permissions (read-only or writeable) through separate metadata files. The server handles multiple client connections using threads, so multiple users can be working with files at the same time. It’s all done in C, using basic system calls and standard library functions, and laid out in a straightforward way so that it’s easy to follow and experiment with.

```bash
# Build server and client
cd server
make clean && make
mkdir -p server_root
cd ../client
make clean && make


# Start the server:
cd server
./server
```

#### Part 1: File Write

```bash

cd client

# Create a test file locally:
echo 'Hello from Part 1!' > local_part1.txt

# Use the WRITE command to send it to the server:
./rfs WRITE local_part1.txt folder/part1.txt

# Check on the server side that 'folder/part1.txt' was created (from another terminal):
ls ../server/server_root/folder/part1.txt

# If the file exists, Part 1 is successful.
```

#### Part 2: File Read

```bash
# Assuming the file folder/part1.txt already exists on the server from Part 1:
cd client

# Use the GET command to fetch it back from the server:
./rfs GET folder/part1.txt downloaded_part1.txt

# Check the downloaded file:
cat downloaded_part1.txt
# We should see "Hello from Part 1!"

```

#### Part 3: File Delete

```bash
# From the client directory:
cd client

# Remove the file we previously uploaded:
./rfs RM folder/part1.txt

# Check on the server side if the file is gone:
ls ../server/server_root/folder/

# 'part1.txt' should no longer be listed.
```

#### Part 4a: Multiple Clients

```bash
# From the client directory:
cd client

# Create multiple test files:
echo 'Multi-client test 1' > multi1.txt
echo 'Multi-client test 2' > multi2.txt

# Run two WRITE commands in parallel:
./rfs WRITE multi1.txt folder/multi1.txt W &
./rfs WRITE multi2.txt folder/multi2.txt W &
wait

# Both files should now be on the server:
ls ../server/server_root/folder/multi1.txt
ls ../server/server_root/folder/multi2.txt

```

#### Part 4b: Permissions

```bash
# From the client directory:
cd client

# Create a file with read-only permission:
echo "This is a read-only file" > ro_test.txt
./rfs WRITE ro_test.txt folder/read_only.txt R

# Attempt to overwrite the read-only file (should fail):
echo "New content attempt" > new_data.txt
./rfs WRITE new_data.txt folder/read_only.txt

# Attempt to remove the read-only file (should fail):
./rfs RM folder/read_only.txt

```

#### Part 4c: Encryption

```bash
# From the client directory:
cd client

# Write a file that will be encrypted on the server:
echo "Sensitive data" > enc.txt
./rfs WRITE enc.txt folder/encfile.txt W

# GET the file back and verify it matches the original content:
./rfs GET folder/encfile.txt decrypted_enc.txt
cat decrypted_enc.txt
# We should see "Sensitive data"

```
