import sys
import crypt

if __name__ == '__main__':
    auths = dict()
    with open(sys.argv[1]) as fh:
        line = fh.readline()
        while line:
            username, password = line.strip().split(':')
            auths[username] = crypt.crypt(password)
            line = fh.readline()
    with open(sys.argv[1], 'w+') as fh:
        fh.writelines(
            [f'{":".join([key, val])}\n' for key, val in auths.items()]
        )
