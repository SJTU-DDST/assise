/***** the library-wide header file *****/
#include "../liblfds710_internal.h"

/***** enums *****/
enum lfds710_btree_au_move
{
  LFDS710_BTREE_AU_MOVE_INVALID,
  LFDS710_BTREE_AU_MOVE_SMALLEST_FROM_RIGHT_CHILD,
  LFDS710_BTREE_AU_MOVE_LARGEST_FROM_LEFT_CHILD,
  LFDS710_BTREE_AU_MOVE_GET_PARENT,
  LFDS710_BTREE_AU_MOVE_MOVE_UP_TREE
};

enum lfds710_btree_au_delete_action
{
  LFDS710_BTREE_AU_DELETE_SELF,
  LFDS710_BTREE_AU_DELETE_SELF_REPLACE_WITH_LEFT_CHILD,
  LFDS710_BTREE_AU_DELETE_SELF_REPLACE_WITH_RIGHT_CHILD,
  LFDS710_BTREE_AU_DELETE_MOVE_LEFT
};

/***** private prototypes *****/

