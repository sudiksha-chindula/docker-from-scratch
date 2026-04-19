import os

greeting = os.getenv('GREETING', 'Default')
print(f"{greeting} from inside the isolated Docksmith container!")

# Attempt to write a file to test isolation 
with open("/app/test.txt", "w") as f:
    f.write("This should not appear on the host system.")# change
