#! /usr/bin/python3

"""
This script generates a C array from the binary dump of an
executable library binary. 

"""

import sys
import os

def main():
  """
  Generate C array by reading binary file
  """
  if len(sys.argv) <= 2:
    print("Need path to binary file and library name.")
    print("Exiting...")
    sys.exit()

  filepath = sys.argv[1]
  libname = sys.argv[2]

  if not os.path.isfile(filepath):
    print("File {} does not exist. Exiting...".format(filepath))
    sys.exit()

  print("/** Autogenerated file */")
  print("#include <stdint.h>")
  print("const uint8_t __{}_blob[] = {}".format(libname, "{")) 


  with open(filepath, "rb") as f:
    bytesread = f.read()

  count = 0

  for b in bytesread:
    print("0x{:02x}".format(b) + ",",end='')
    count = count + 1
    if count % 16 == 0:
      print("")

  print("\n};")

if __name__ == '__main__':
   main()
