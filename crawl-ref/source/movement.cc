/**
 * @file
 * @brief Movement, open-close door commands, movement effects.
**/

#include <algorithm>
#include <cstring>
#include <string>
#include <sstream>

#include "AppHdr.h"

#include "movement.h"

#include "abyss.h"
#include "bloodspatter.h"
#include "cloud.h"
#include "coord.h"
#include "coordit.h"
#include "delay.h"
#include "directn.h"
#include "dungeon.h"
#include "env.h"
#include "fight.h"
#include "fprop.h"
#include "god-abil.h"
#include "god-conduct.h"
#include "god-passive.h"
#include "items.h"
#include "message.h"
#include "mon-act.h"
#include "mon-death.h"
#include "mon-place.h"
#include "mon-util.h"
#include "player.h"
#include "player-reacts.h"
#include "prompt.h"
#include "random.h"
#include "religion.h"
#include "shout.h"
#include "state.h"
#include "stringutil.h"
#include "spl-damage.h"
#include "spl-selfench.h" // noxious_bog_cell
#include "target-compass.h"
#include "terrain.h"
#include "traps.h"
#include "travel.h"
#include "transform.h"
#include "xom.h" // XOM_CLOUD_TRAIL_TYPE_KEY

static void _apply_move_time_taken(int additional_time_taken = 0);

// Swap monster to this location. Player is swapped elsewhere.
// Moves the monster into position, but does not move the player
// or apply location effects: the latter should happen after the
// player is moved.
static void _swap_places(monster* mons, const coord_def &loc)
{
    ASSERT(map_bounds(loc));
    ASSERT(monster_habitable_grid(mons, grd(loc)));

    if (monster_at(loc))
    {
        if (mons->type == MONS_WANDERING_MUSHROOM
            && monster_at(loc)->type == MONS_TOADSTOOL)
        {
            // We'll fire location effects for 'mons' back in move_player_action,
            // so don't do so here. The toadstool won't get location effects,
            // but the player will trigger those soon enough. This wouldn't
            // work so well if toadstools were aquatic, or were
            // otherwise handled specially in monster_swap_places or in
            // apply_location_effects.
            monster_swaps_places(mons, loc - mons->pos(), true, false);
            return;
        }
        else
        {
            mpr("Something prevents you from swapping places.");
            return;
        }
    }

    // Friendly foxfire dissipates instead of damaging the player.
    if (mons->type == MONS_FOXFIRE)
    {
        simple_monster_message(*mons, " dissipates!",
                               MSGCH_MONSTER_DAMAGE, MDAM_DEAD);
        monster_die(*mons, KILL_DISMISSED, NON_MONSTER, true);
        return;
    }

    mpr("You swap places.");

    mons->move_to_pos(loc, true, true);

    return;
}

// Check squares adjacent to player for given feature and return how
// many there are. If there's only one, return the dx and dy.
static int _check_adjacent(dungeon_feature_type feat, coord_def& delta)
{
    int num = 0;

    set<coord_def> doors;
    for (adjacent_iterator ai(you.pos(), true); ai; ++ai)
    {
        if (grd(*ai) == feat)
        {
            // Specialcase doors to take into account gates.
            if (feat_is_door(feat))
            {
                // Already included in a gate, skip this door.
                if (doors.count(*ai))
                    continue;

                // Check if it's part of a gate. If so, remember all its doors.
                set<coord_def> all_door;
                find_connected_identical(*ai, all_door);
                doors.insert(begin(all_door), end(all_door));
            }

            num++;
            delta = *ai - you.pos();
        }
    }

    return num;
}

static void _entered_malign_portal(actor* act)
{
    ASSERT(act); // XXX: change to actor &act
    if (you.can_see(*act))
    {
        mprf("%s %s twisted violently and ejected from the portal!",
             act->name(DESC_THE).c_str(), act->conj_verb("be").c_str());
    }

    act->blink();
    act->hurt(nullptr, roll_dice(2, 4), BEAM_MISSILE, KILLED_BY_WILD_MAGIC,
              "", "entering a malign gateway");
}

bool cancel_barbed_move(bool lunging)
{
    if (you.duration[DUR_BARBS] && !you.props.exists(BARBS_MOVE_KEY))
    {
        std::string prompt = "The barbs in your skin will harm you if you move.";
        prompt += lunging ? " Lunging like this could really hurt!" : "";
        prompt += " Continue?";
        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return true;
        }

        you.props[BARBS_MOVE_KEY] = true;
    }

    return false;
}

void apply_barbs_damage(bool lunging)
{
    if (you.duration[DUR_BARBS])
    {
        mprf(MSGCH_WARN, "The barbed spikes dig painfully into your body "
                         "as you move.");
        ouch(roll_dice(2, you.attribute[ATTR_BARBS_POW]), KILLED_BY_BARBS);
        bleed_onto_floor(you.pos(), MONS_PLAYER, 2, false);

        // Sometimes decrease duration even when we move.
        if (one_chance_in(3))
            extract_manticore_spikes("The barbed spikes snap loose.");
        // But if that failed to end the effect, duration stays the same.
        if (you.duration[DUR_BARBS])
            you.duration[DUR_BARBS] += (lunging ? 0 : you.time_taken);
    }
}

void remove_ice_armour_movement()
{
    if (you.duration[DUR_ICY_ARMOUR])
    {
        mprf(MSGCH_DURATION, "Your icy armour cracks and falls away as "
                             "you move.");
        you.duration[DUR_ICY_ARMOUR] = 0;
        you.redraw_armour_class = true;
    }
}

void remove_water_hold()
{
    if (you.duration[DUR_WATER_HOLD])
    {
        mpr("You slip free of the water engulfing you.");
        you.props.erase("water_holder");
        you.clear_far_engulf();
    }
}

static void _clear_constriction_data()
{
    you.stop_directly_constricting_all(true);
    if (you.is_directly_constricted())
        you.stop_being_constricted();
}

void apply_noxious_bog(const coord_def old_pos)
{
    if (you.duration[DUR_NOXIOUS_BOG])
    {
        if (cell_is_solid(old_pos))
            ASSERT(you.wizmode_teleported_into_rock);
        else
            noxious_bog_cell(old_pos);
    }
}

bool apply_cloud_trail(const coord_def old_pos)
{
    if (you.duration[DUR_CLOUD_TRAIL])
    {
        if (cell_is_solid(old_pos))
            ASSERT(you.wizmode_teleported_into_rock);
        else
        {
            auto cloud = static_cast<cloud_type>(
                you.props[XOM_CLOUD_TRAIL_TYPE_KEY].get_int());
            ASSERT(cloud != CLOUD_NONE);
            check_place_cloud(cloud, old_pos, random_range(3, 10), &you,
                              0, -1);
            return true;
        }
    }
    return false;
}

bool cancel_confused_move(bool stationary)
{
    dungeon_feature_type dangerous = DNGN_FLOOR;
    monster *bad_mons = 0;
    string bad_suff, bad_adj;
    bool penance = false;
    bool flight = false;
    for (adjacent_iterator ai(you.pos(), false); ai; ++ai)
    {
        if (!stationary
            && is_feat_dangerous(grd(*ai), true)
            && need_expiration_warning(grd(*ai))
            && (dangerous == DNGN_FLOOR || grd(*ai) == DNGN_LAVA))
        {
            dangerous = grd(*ai);
            if (need_expiration_warning(DUR_FLIGHT, grd(*ai)))
                flight = true;
            break;
        }
        else
        {
            string suffix, adj;
            monster *mons = monster_at(*ai);
            if (mons
                && (stationary
                    || !(is_sanctuary(you.pos()) && is_sanctuary(mons->pos()))
                       && !fedhas_passthrough(mons))
                && bad_attack(mons, adj, suffix, penance)
                && mons->angered_by_attacks())
            {
                bad_mons = mons;
                bad_suff = suffix;
                bad_adj = adj;
                if (penance)
                    break;
            }
        }
    }

    if (dangerous != DNGN_FLOOR || bad_mons)
    {
        string prompt = "";
        prompt += "Are you sure you want to ";
        prompt += !stationary ? "stumble around" : "swing wildly";
        prompt += " while confused and next to ";

        if (dangerous != DNGN_FLOOR)
        {
            prompt += (dangerous == DNGN_LAVA ? "lava" : "deep water");
            prompt += flight ? " while you are losing your buoyancy"
                             : " while your transformation is expiring";
        }
        else
        {
            string name = bad_mons->name(DESC_PLAIN);
            if (starts_with(name, "the "))
               name.erase(0, 4);
            if (!starts_with(bad_adj, "your"))
               bad_adj = "the " + bad_adj;
            prompt += bad_adj + name + bad_suff;
        }
        prompt += "?";

        if (penance)
            prompt += " This could place you under penance!";

        if (!crawl_state.disables[DIS_CONFIRMATIONS]
            && !yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return true;
        }
    }

    return false;
}

// Opens doors.
// If move is !::origin, it carries a specific direction for the
// door to be opened (eg if you type ctrl + dir).
void open_door_action(coord_def move)
{
    ASSERT(!crawl_state.game_is_arena());
    ASSERT(!crawl_state.arena_suspended);

    if (you.attribute[ATTR_HELD])
    {
        free_self_from_net();
        you.turn_is_over = true;
        return;
    }

    if (you.confused())
    {
        canned_msg(MSG_TOO_CONFUSED);
        return;
    }

    coord_def delta;

    // The player hasn't picked a direction yet.
    if (move.origin())
    {
        const int num = _check_adjacent(DNGN_CLOSED_DOOR, move)
                        + _check_adjacent(DNGN_CLOSED_CLEAR_DOOR, move)
                        + _check_adjacent(DNGN_RUNED_DOOR, move)
                        + _check_adjacent(DNGN_RUNED_CLEAR_DOOR, move);

        if (num == 0)
        {
            mpr("There's nothing to open nearby.");
            return;
        }

        // If there's only one door to open, don't ask.
        if (num == 1 && Options.easy_door)
            delta = move;
        else
        {
            delta = prompt_compass_direction();
            if (delta.origin())
                return;
        }
    }
    else
        delta = move;

    // We got a valid direction.
    const coord_def doorpos = you.pos() + delta;

    if (door_vetoed(doorpos))
    {
        // Allow doors to be locked.
        const string door_veto_message = env.markers.property_at(doorpos,
                                                                 MAT_ANY,
                                                                 "veto_reason");
        if (door_veto_message.empty())
            mpr("The door is shut tight!");
        else
            mpr(door_veto_message);
        if (you.confused())
            you.turn_is_over = true;

        return;
    }

    const dungeon_feature_type feat = (in_bounds(doorpos) ? grd(doorpos)
                                                          : DNGN_UNSEEN);
    switch (feat)
    {
    case DNGN_CLOSED_DOOR:
    case DNGN_CLOSED_CLEAR_DOOR:
    case DNGN_RUNED_DOOR:
    case DNGN_RUNED_CLEAR_DOOR:
        player_open_door(doorpos);
        break;
    case DNGN_OPEN_DOOR:
    case DNGN_OPEN_CLEAR_DOOR:
    {
        string door_already_open = "";
        if (in_bounds(doorpos))
        {
            door_already_open = env.markers.property_at(doorpos, MAT_ANY,
                                                    "door_verb_already_open");
        }

        if (!door_already_open.empty())
            mpr(door_already_open);
        else
            mpr("It's already open!");
        break;
    }
    case DNGN_SEALED_DOOR:
    case DNGN_SEALED_CLEAR_DOOR:
        mpr("That door is sealed shut!");
        break;
    default:
        mpr("There isn't anything that you can open there!");
        break;
    }
}

void close_door_action(coord_def move)
{
    if (you.attribute[ATTR_HELD])
    {
        mprf("You can't close doors while %s.", held_status());
        return;
    }

    if (you.confused())
    {
        canned_msg(MSG_TOO_CONFUSED);
        return;
    }

    coord_def delta;

    if (move.origin())
    {
        // If there's only one door to close, don't ask.
        int num = _check_adjacent(DNGN_OPEN_DOOR, move)
                  + _check_adjacent(DNGN_OPEN_CLEAR_DOOR, move);
        if (num == 0)
        {
            mpr("There's nothing to close nearby.");
            return;
        }
        // move got set in _check_adjacent
        else if (num == 1 && Options.easy_door)
            delta = move;
        else
        {
            delta = prompt_compass_direction();
            if (delta.origin())
                return;
        }
    }
    else
        delta = move;

    const coord_def doorpos = you.pos() + delta;
    const dungeon_feature_type feat = (in_bounds(doorpos) ? grd(doorpos)
                                                          : DNGN_UNSEEN);

    switch (feat)
    {
    case DNGN_OPEN_DOOR:
    case DNGN_OPEN_CLEAR_DOOR:
        player_close_door(doorpos);
        break;
    case DNGN_CLOSED_DOOR:
    case DNGN_CLOSED_CLEAR_DOOR:
    case DNGN_RUNED_DOOR:
    case DNGN_RUNED_CLEAR_DOOR:
    case DNGN_SEALED_DOOR:
    case DNGN_SEALED_CLEAR_DOOR:
        mpr("It's already closed!");
        break;
    default:
        mpr("There isn't anything that you can close there!");
        break;
    }
}

// Maybe prompt to enter a portal, return true if we should enter the
// portal, false if the user said no at the prompt.
bool prompt_dangerous_portal(dungeon_feature_type ftype)
{
    switch (ftype)
    {
    case DNGN_ENTER_PANDEMONIUM:
    case DNGN_ENTER_ABYSS:
        return yesno("If you enter this portal you might not be able to return "
                     "immediately. Continue?", false, 'n');

    case DNGN_MALIGN_GATEWAY:
        return yesno("Are you sure you wish to approach this portal? There's no "
                     "telling what its forces would wreak upon your fragile "
                     "self.", false, 'n');

    default:
        return true;
    }
}

/**
 * Lunges the player toward a hostile monster, if one exists in the direction of
 * the move input. Invalid things along the Lunge path cancel the Lunge.
 *
 * @param move  A relative coord_def of the player's CMD_MOVE input,
 *              as called by move_player_action().
 * @return      spret::fail if something invalid prevented the lunge,
 *              spret::abort if a player prompt response should cancel the move
 *              entirely,
 *              spret::success if the lunge occurred.
 */
static spret _lunge_forward(coord_def move)
{
    ASSERT(!crawl_state.game_is_arena());

    // Assert if the requested move is beyond [-1,1] distance,
    // this would throw off our tracer_target.
    ASSERT(abs(you.pos().x - move.x) > 1 || abs(you.pos().y - move.y) > 1);

    if (crawl_state.is_repeating_cmd())
    {
        crawl_state.cant_cmd_repeat("You can't repeat lunge.");
        crawl_state.cancel_cmd_again();
        crawl_state.cancel_cmd_repeat();
        return spret::fail;
    }

    // Don't lunge if the player has status effects that should prevent it:
    // fungusform + terrified, confusion, immobile (tree)form, or constricted.
    if (you.is_nervous()
        || you.confused()
        || you.is_stationary()
        || you.is_constricted())
    {
        return spret::fail;
    }

    const int tracer_range = 7;
    const int lunge_distance = 1;

    // This logic assumes that the relative coord_def move is from [-1,1].
    // If the move_player_action() calls are ever rewritten in a way that
    // breaks this assumption, these targeters will need to be updated.
    const coord_def tracer_target = you.pos() + (move * tracer_range);
    const coord_def lunge_target = you.pos() + (move * lunge_distance);

    // Setup the lunge tracer beam.
    bolt beam;
    beam.range           = LOS_RADIUS;
    beam.aimed_at_spot   = true;
    beam.target          = tracer_target;
    beam.name            = "lunging";
    beam.source_name     = "you";
    beam.source          = you.pos();
    beam.source_id       = MID_PLAYER;
    beam.thrower         = KILL_YOU;
    // The lunge reposition is explicitly noiseless for stab synergy.
    // Its ensuing move or attack action will generate a normal amount of noise.
    beam.loudness        = 0;
    beam.pierce          = true;
    beam.affects_nothing = true;
    beam.is_tracer       = true;
    // is_targeting prevents bolt::do_fire() from interrupting with a prompt,
    // if our tracer crosses something that blocks line of fire.
    beam.is_targeting    = true;
    beam.fire();

    const monster* valid_target = nullptr;

    // Iterate the tracer to see if the first visible target is a hostile mons.
    for (coord_def p : beam.path_taken)
    {
        // Don't lunge if our tracer path is broken by deep water, lava,
        // teleport traps, etc., before it reaches a monster.
        if (!feat_is_traversable(grd(p))
            && !(grd(p) == DNGN_SHALLOW_WATER))
        {
            break;
        }
        // Don't lunge if the tracer path is broken by something solid or
        // transparent: doors, grates, etc.
        if (cell_is_solid(p) || you.trans_wall_blocking(p))
            break;

        const monster* mon = monster_at(p);
        if (!mon)
            continue;
        // Don't lunge at invis mons, but allow the tracer to keep going.
        else if (mon && !you.can_see(*mon))
            continue;
        // Don't lunge if the closest mons is non-hostile or a plant.
        else if (mon && (mon->friendly()
                         || mon->neutral()
                         || mons_is_firewood(*mon)))
        {
            break;
        }
        // Okay, the first mons along the tracer is a valid target.
        else if (mon)
        {
            valid_target = mon;
            break;
        }
    }
    if (!valid_target)
        return spret::fail;

    // Reset the beam target to the actual lunge_target distance.
    beam.target = lunge_target;

    // Don't lunge if the player's tile is being targeted, somehow.
    if (beam.target == you.pos())
        return spret::fail;

    // Don't lunge if it would take us away from a beholder.
    const monster* beholder = you.get_beholder(beam.target);
    if (beholder)
    {
        clear_messages();
        mprf("You cannot lunge away from %s!",
             beholder->name(DESC_THE, true).c_str());
        return spret::fail;
    }

    // Don't lunge if it would take us toward a fearmonger.
    const monster* fearmonger = you.get_fearmonger(beam.target);
    if (fearmonger)
    {
        clear_messages();
        mprf("You cannot lunge closer to %s!",
             fearmonger->name(DESC_THE, true).c_str());
        return spret::fail;
    }

    // Don't lunge if it would land us on top of a monster.
    const monster* mons = monster_at(beam.target);
    if (mons)
    {
        if (!you.can_see(*mons))
        {
            // .. if it was in the way and invisible, notify the player.
            clear_messages();
            mpr("Something unexpectedly blocked you, preventing you from lunging!");
        }
        return spret::fail;
    }

    // Don't lunge if the target tile has a dangerous (!FFT_SOLID) feature:
    if (feat_is_lava(grd(beam.target)))
        return spret::fail;
    else if (grd(beam.target) == DNGN_DEEP_WATER
             || grd(beam.target) == DNGN_TOXIC_BOG)
    {
        return spret::fail;
    }
    // Don't lunge if the target tile is out of bounds,
    // Don't lunge if we cannot see the target tile,
    // Don't lunge if something transparent is in the way.
    else if (you.trans_wall_blocking(beam.target))
        return spret::fail;
    // Don't lunge if the target tile has a feature with the FFT_SOLID flag:
    // (see feature-data.h)
    // This covers walls, closed doors, sealed doors, trees, open sea, lava sea,
    // endless salt, grates, statues, malign gateways, and DNGN_UNSEEN.
    else if (cell_is_solid(beam.target))
        return spret::fail;

    // Abort if the player answers no to a dangerous terrain/trap/cloud/
    // exclusion prompt; messaging for this is handled by check_moveto().
    if (!check_moveto(beam.target, "lunge"))
        return spret::abort;

    // Abort if the player answers no to a DUR_BARBS damaging move prompt.
    if (cancel_barbed_move(true))
        return spret::abort;

    // We've passed the validity checks, go ahead and lunge.

    // First, apply any necessary pre-move effects:
    remove_water_hold();
    _clear_constriction_data();
    const coord_def old_pos = you.pos();

    clear_messages();
    mprf("You lunge towards %s!", valid_target->name(DESC_THE, true).c_str());
    // stepped = true, we're flavouring this as movement, not a blink.
    move_player_to_grid(beam.target, true);

    // Lastly, apply post-move effects unhandled by move_player_to_grid().
    apply_barbs_damage(true);
    remove_ice_armour_movement();
    apply_noxious_bog(old_pos);
    apply_cloud_trail(old_pos);

    // If there is somehow an active run delay here, update the travel trail.
    if (you_are_delayed() && current_delay()->is_run())
        env.travel_trail.push_back(you.pos());

    return spret::success;
}

static void _apply_move_time_taken(int additional_time_taken)
{
    you.time_taken *= player_movement_speed();
    you.time_taken = div_rand_round(you.time_taken, 10);
    you.time_taken += additional_time_taken;

    if (you.running && you.running.travel_speed)
    {
        you.time_taken = max(you.time_taken,
                             div_round_up(100, you.running.travel_speed));
    }

    if (you.duration[DUR_NO_HOP])
        you.duration[DUR_NO_HOP] += you.time_taken;
}

// The "first square" of lunging ordinarily has no time cost, and the
// "second square" is where its move delay or attack delay would be applied.
// If the player begins a lunge, and then cancels the second move, as through a
// prompt, we have to ensure they don't get zero-cost movement out of it.
// Here we apply movedelay, end the turn, and call relevant post-move effects.
static void _finalize_cancelled_lunge_move(coord_def initial_position)
{
    _apply_move_time_taken();   // tanstaaf-lunge
    you.turn_is_over = true;

    if (player_in_branch(BRANCH_ABYSS))
        maybe_shift_abyss_around_player();

    you.apply_berserk_penalty = true;

    // lunging is pretty dang hasty
    if (you_worship(GOD_CHEIBRIADOS) && one_chance_in(2))
        did_god_conduct(DID_HASTY, 1, true);

    bool did_wu_jian_attack = false;
    if (you_worship(GOD_WU_JIAN))
        did_wu_jian_attack = wu_jian_post_move_effects(false, initial_position);

    // We're eligible for acrobat if we don't trigger WJC attacks.
    if (!did_wu_jian_attack)
        update_acrobat_status();
}

// Called when the player moves by walking/running. Also calls attack
// function etc when necessary.
void move_player_action(coord_def move)
{
    ASSERT(!crawl_state.game_is_arena() && !crawl_state.arena_suspended);

    bool attacking = false;
    bool moving = true;         // used to prevent eventual movement (swap)
    bool swap = false;

    int additional_time_taken = 0; // Extra time independent of movement speed

    ASSERT(!in_bounds(you.pos()) || !cell_is_solid(you.pos())
           || you.wizmode_teleported_into_rock);

    if (you.attribute[ATTR_HELD])
    {
        free_self_from_net();
        you.turn_is_over = true;
        return;
    }

    coord_def initial_position = you.pos();

    // When confused, sometimes make a random move.
    if (you.confused())
    {
        if (you.is_stationary())
        {
            // Don't choose a random location to try to attack into - allows
            // abuse, since trying to move (not attack) takes no time, and
            // shouldn't. Just force confused trees to use ctrl.
            mpr("You cannot move. (Use ctrl+direction or * direction to "
                "attack without moving.)");
            return;
        }

        if (cancel_confused_move(false))
            return;

        if (cancel_barbed_move())
            return;

        if (!one_chance_in(3))
        {
            move.x = random2(3) - 1;
            move.y = random2(3) - 1;
            if (move.origin())
            {
                mpr("You're too confused to move!");
                you.apply_berserk_penalty = true;
                you.turn_is_over = true;
                return;
            }
        }

        const coord_def new_targ = you.pos() + move;
        if (!in_bounds(new_targ) || !you.can_pass_through(new_targ))
        {
            you.turn_is_over = true;
            if (you.digging) // no actual damage
            {
                mprf("Your mandibles retract as you bump into %s.",
                     feature_description_at(new_targ, false,
                                            DESC_THE).c_str());
                you.digging = false;
            }
            else
            {
                mprf("You bump into %s.",
                     feature_description_at(new_targ, false,
                                            DESC_THE).c_str());
            }
            you.apply_berserk_penalty = true;
            crawl_state.cancel_cmd_repeat();

            return;
        }
    }

    bool lunged = false;

    if (you.lunging())
    {
        switch (_lunge_forward(move))
        {
            // Check the player's position again; lunge may have moved us.

            // Cancel the move entirely if lunge was aborted from a prompt.
            case spret::abort:
                ASSERT(!in_bounds(you.pos()) || !cell_is_solid(you.pos())
                       || you.wizmode_teleported_into_rock);
                return;

            case spret::success:
                lunged = true;
                // If we've lunged, reset initial_position for WJC targeting.
                initial_position = you.pos();
                // intentional fallthrough
            default:
            case spret::fail:
                ASSERT(!in_bounds(you.pos()) || !cell_is_solid(you.pos())
                       || you.wizmode_teleported_into_rock);
                break;
        }
    }

    const coord_def targ = you.pos() + move;
    // You can't walk out of bounds!
    if (!in_bounds(targ))
    {
        // Why isn't the border permarock?
        if (you.digging)
            mpr("This wall is too hard to dig through.");
        return;
    }

    const string walkverb = you.airborne()                     ? "fly"
                          : you.swimming()                     ? "swim"
                          : you.form == transformation::spider ? "crawl"
                          : (you.species == SP_NAGA
                             && form_keeps_mutations())        ? "slither"
                                                               : "walk";

    monster* targ_monst = monster_at(targ);
    if (fedhas_passthrough(targ_monst) && !you.is_stationary())
    {
        // Moving on a plant takes 1.5 x normal move delay. We
        // will print a message about it but only when moving
        // from open space->plant (hopefully this will cut down
        // on the message spam).
        you.time_taken = div_rand_round(you.time_taken * 3, 2);

        monster* current = monster_at(you.pos());
        if (!current || !fedhas_passthrough(current))
        {
            // Probably need a better message. -cao
            mprf("You %s carefully through the %s.", walkverb.c_str(),
                 mons_genus(targ_monst->type) == MONS_FUNGUS ? "fungus"
                                                             : "plants");
        }
        targ_monst = nullptr;
    }

    bool targ_pass = you.can_pass_through(targ) && !you.is_stationary();

    if (you.digging)
    {
        if (feat_is_diggable(grd(targ)))
            targ_pass = true;
        else // moving or attacking ends dig
        {
            you.digging = false;
            if (feat_is_solid(grd(targ)))
                mpr("You can't dig through that.");
            else
                mpr("You retract your mandibles.");
        }
    }

    // You can swap places with a friendly or good neutral monster if
    // you're not confused, or even with hostiles if both of you are inside
    // a sanctuary.
    const bool try_to_swap = targ_monst
                             && (targ_monst->wont_attack()
                                    && !you.confused()
                                 || is_sanctuary(you.pos())
                                    && is_sanctuary(targ));

    // You cannot move away from a siren but you CAN fight monsters on
    // neighbouring squares.
    monster* beholder = nullptr;
    if (!you.confused())
        beholder = you.get_beholder(targ);

    // You cannot move closer to a fear monger.
    monster *fmonger = nullptr;
    if (!you.confused())
        fmonger = you.get_fearmonger(targ);

    if (you.running.check_stop_running())
    {
        // If we cancel this move after lunging, we end the turn.
        if (lunged)
        {
            move.reset();
            _finalize_cancelled_lunge_move(initial_position);
            return;
        }
        // [ds] Do we need this? Shouldn't it be false to start with?
        you.turn_is_over = false;
        return;
    }

    coord_def mon_swap_dest;

    if (targ_monst && !targ_monst->submerged())
    {
        if (try_to_swap && !beholder && !fmonger)
        {
            if (swap_check(targ_monst, mon_swap_dest))
                swap = true;
            else
            {
                stop_running();
                moving = false;
            }
        }
        else if (targ_monst->temp_attitude() == ATT_NEUTRAL && !you.confused()
                 && targ_monst->visible_to(&you))
        {
            simple_monster_message(*targ_monst, " refuses to make way for you. "
                              "(Use ctrl+direction or * direction to attack.)");
            you.turn_is_over = false;
            return;
        }
        else if (!try_to_swap) // attack!
        {
            // Don't allow the player to freely locate invisible monsters
            // with confirmation prompts.
            if (!you.can_see(*targ_monst)
                && !you.confused()
                && !check_moveto(targ, walkverb))
            {
                stop_running();
                // If we cancel this move after lunging, we end the turn.
                if (lunged)
                {
                    move.reset();
                    _finalize_cancelled_lunge_move(initial_position);
                    return;
                }
                you.turn_is_over = false;
                return;
            }

            you.turn_is_over = true;
            fight_melee(&you, targ_monst);

            you.berserk_penalty = 0;
            attacking = true;
        }
    }
    else if (you.form == transformation::fungus && moving && !you.confused())
    {
        if (you.is_nervous())
        {
            mpr("You're too terrified to move while being watched!");
            stop_running();
            you.turn_is_over = false;
            return;
        }
    }

    const bool running = you_are_delayed() && current_delay()->is_run();

    if (!attacking && targ_pass && moving && !beholder && !fmonger)
    {
        if (you.confused() && is_feat_dangerous(env.grid(targ)))
        {
            mprf("You nearly stumble into %s!",
                 feature_description_at(targ, false, DESC_THE).c_str());
            you.apply_berserk_penalty = true;
            you.turn_is_over = true;
            return;
        }

        if (!you.confused() && !check_moveto(targ, walkverb))
        {
            stop_running();
            // If we cancel this move after lunging, we end the turn.
            if (lunged)
            {
                move.reset();
                _finalize_cancelled_lunge_move(initial_position);
                return;
            }
            you.turn_is_over = false;
            return;
        }

        // If confused, we've already been prompted (in case of stumbling into
        // a monster and attacking instead).
        if (!you.confused() && cancel_barbed_move())
            return;

        if (!you.attempt_escape()) // false means constricted and did not escape
            return;

        if (you.digging)
        {
            mprf("You dig through %s.", feature_description_at(targ, false,
                 DESC_THE).c_str());
            destroy_wall(targ);
            noisy(6, you.pos());
            additional_time_taken += BASELINE_DELAY / 5;
        }

        if (swap)
            _swap_places(targ_monst, mon_swap_dest);

        if (running && env.travel_trail.empty())
            env.travel_trail.push_back(you.pos());
        else if (!running)
            clear_travel_trail();

        coord_def old_pos = you.pos();
        // Don't trigger things that require movement
        // when confusion causes no move.
        if (you.pos() != targ && targ_pass)
        {
            remove_water_hold();
            _clear_constriction_data();
            move_player_to_grid(targ, true);
            apply_barbs_damage();
            remove_ice_armour_movement();
            apply_noxious_bog(old_pos);
            apply_cloud_trail(old_pos);
        }

        // Now it is safe to apply the swappee's location effects and add
        // trailing effects. Doing so earlier would allow e.g. shadow traps to
        // put a monster at the player's location.
        if (swap)
            targ_monst->apply_location_effects(targ);

        if (you_are_delayed() && current_delay()->is_run())
            env.travel_trail.push_back(you.pos());

        _apply_move_time_taken(additional_time_taken);

        move.reset();
        you.turn_is_over = true;
        request_autopickup();
    }

    // BCR - Easy doors single move
    if ((Options.travel_open_doors || !you.running)
        && !attacking
        && feat_is_closed_door(grd(targ)))
    {
        open_door_action(move);
        move.reset();
        return;
    }
    else if (!targ_pass && grd(targ) == DNGN_MALIGN_GATEWAY
             && !attacking && !you.is_stationary())
    {
        if (!crawl_state.disables[DIS_CONFIRMATIONS]
            && !prompt_dangerous_portal(grd(targ)))
        {
            return;
        }

        move.reset();
        you.turn_is_over = true;

        _entered_malign_portal(&you);
        return;
    }
    else if (!targ_pass && !attacking)
    {
        if (you.is_stationary())
            canned_msg(MSG_CANNOT_MOVE);
        else if (grd(targ) == DNGN_OPEN_SEA)
            mpr("The ferocious winds and tides of the open sea thwart your progress.");
        else if (grd(targ) == DNGN_LAVA_SEA)
            mpr("The endless sea of lava is not a nice place.");
        else if (feat_is_tree(grd(targ)) && you_worship(GOD_FEDHAS))
            mpr("You cannot walk through the dense trees.");

        stop_running();
        move.reset();
        you.turn_is_over = false;
        crawl_state.cancel_cmd_repeat();
        return;
    }
    else if (beholder && !attacking)
    {
        mprf("You cannot move away from %s!",
            beholder->name(DESC_THE).c_str());
        stop_running();
        return;
    }
    else if (fmonger && !attacking)
    {
        mprf("You cannot move closer to %s!",
            fmonger->name(DESC_THE).c_str());
        stop_running();
        return;
    }

    if (you.running == RMODE_START)
        you.running = RMODE_CONTINUE;

    if (player_in_branch(BRANCH_ABYSS))
        maybe_shift_abyss_around_player();

    you.apply_berserk_penalty = !attacking;

    if (!attacking
        && you_worship(GOD_CHEIBRIADOS)
        && ((one_chance_in(10) && you.run())
             || (one_chance_in(2) && lunged)))
    {
        did_god_conduct(DID_HASTY, 1, true);
    }

    bool did_wu_jian_attack = false;
    if (you_worship(GOD_WU_JIAN) && !attacking)
        did_wu_jian_attack = wu_jian_post_move_effects(false, initial_position);

    // If you actually moved you are eligible for amulet of the acrobat.
    if (!attacking && moving && !did_wu_jian_attack)
        update_acrobat_status();
}
