// A room floor holding a thousand objects is legitimate -- a mass quit drops every quitter's gear in
// the room they quit in. obj_to_room's cycle guard treated it as corruption.

#include "../handler.h"
#include "../structs.h"
#include "../utils.h"

#include <gtest/gtest.h>

#include <vector>

extern struct room_data world;
extern int top_of_world;
extern struct descriptor_data* descriptor_list;
void clear_object(struct obj_data* obj);

namespace {

TEST(RoomContents, AddingAnObjectToAFloorOfAThousandKeepsEveryObjectListed)
{
    if (room_data::BASE_WORLD == nullptr)
        world.create_bulk(1);
    const int saved_top_of_world = top_of_world;
    const descriptor_data* saved_descriptor_list = descriptor_list;
    descriptor_list = nullptr; // obj_to_room mudlogs; no stale descriptors from earlier tests
    top_of_world = 0;
    obj_data* saved_contents = world[0].contents;

    // The floor, appended the way extract_char drops a quitting player's inventory: straight onto
    // the list, with no obj_to_room call and so no count check.
    std::vector<obj_data> floor(1000);
    world[0].contents = nullptr;
    for (size_t index = floor.size(); index-- > 0;) {
        clear_object(&floor[index]);
        floor[index].in_room = 0;
        floor[index].next_content = world[0].contents;
        world[0].contents = &floor[index];
    }

    // One more object reaches the floor through obj_to_room -- a quitter's worn gear goes this way.
    obj_data dropped;
    clear_object(&dropped);
    obj_to_room(&dropped, 0);
    dropped.in_room = 0;

    size_t listed = 0;
    for (obj_data* object = world[0].contents; object && listed <= floor.size() + 1; object = object->next_content)
        ++listed;
    EXPECT_EQ(listed, floor.size() + 1)
        << "objects that still claim this room fell off its contents list; extracting one later "
           "(decay in point_update) walks the list, never finds it, and dereferences NULL in obj_from_room";

    world[0].contents = saved_contents;
    top_of_world = saved_top_of_world;
    descriptor_list = const_cast<descriptor_data*>(saved_descriptor_list);
}

} // namespace
