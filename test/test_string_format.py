from testing_helpers import test 

@test
def format_consts():
  return '%d %d %s' % (100, 100, "Hello")

@test
def format_args(a,b):
  return "%s %d" % (a,b)

def test_format():
  format_consts()
  format_args(1.0, 2.0)
  format_args({3:2},2)




