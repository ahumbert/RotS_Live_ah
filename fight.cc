/* ************************************************************************
*   File: fight.c                                       Part of CircleMUD *
*  Usage: Combat system                                                   *
*                                                                         *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  Copyright (C) 1993 by the Trustees of the Johns Hopkins University     *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "platdef.h"

#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "handler.h"
#include "interpre.h"
#include "db.h"
#include "spells.h"
#include "limits.h"
#include "color.h"
#include "script.h"
#include "zone.h"  /* For zone_table */
#include "pkill.h"

#include "char_utils.h"

#define IS_PHYSICAL(_at) \
  ((_at) >= TYPE_HIT && (_at) <= TYPE_CRUSH ? TRUE : FALSE)

/* Structures */
struct char_data *combat_list = 0;       /* head of l-list of fighting chars */
struct char_data *combat_next_dude = 0;  /* Next dude global trick */

/* External structures */
extern struct room_data world;
extern struct message_list fight_messages[MAX_MESSAGES];
extern struct obj_data *object_list;
extern struct char_data * waiting_list; /* in db.cpp */
extern int immort_start_room;
extern int mortal_start_room[];
extern int r_mortal_start_room[];
extern int r_immort_start_room;
extern struct index_data *mob_index;
extern int armor_absorb(struct obj_data *obj);
extern int average_mob_life;
extern skill_data skills[];
extern int max_race_str[];
extern bool weapon_willpower_damage(char_data *ch, char_data *victim);
extern void check_break_prep(struct char_data *);
extern int max_npc_corpse_time, max_pc_corpse_time;
extern char * pc_star_types[];

/* External procedures */
char *fread_string(FILE *fl, char *error);
int check_resistances(char_data * ch, int attacktype);
void stop_hiding(struct char_data *, char);
void break_meditation(char_data *ch);
ACMD(do_flee);
ACMD(do_stand);
ACMD(do_mental);

/* local procedures */
void hit(struct char_data *ch, struct char_data *victim, int type);
void do_parry(struct char_data *ch, struct char_data *victim, int type);
void forget(struct char_data *ch, struct char_data *victim);
void remember(struct char_data *ch, struct char_data *victim);
void complete_delay(struct char_data * ch);
void group_gain(struct char_data *ch, struct char_data *victim);
void do_pass_through(struct char_data *ch, struct char_data *victim, int type);
/* riposte doesn't use int type, but maybe when messages are sorted... */
void do_riposte(struct char_data *ch, struct char_data *victim);
int check_overkill(struct char_data *ch);
int check_hallucinate(struct char_data *ch, struct char_data *victim);

/* Weapon attack texts */
struct attack_hit_type attack_hit_text[] = {
  { "hit",   "hits" },
  { "pound", "pounds" },
  { "pierce", "pierces" },
  { "slash", "slashes" },
  { "stab", "stabs" },
  { "whip", "whips" },
  { "stab", "stabs" },
  { "claw", "claws" },
  { "bite", "bites" },
  { "sting", "stings" },
  { "cleave", "cleaves" },
  { "flail", "flails" },
  { "smite", "smites" },
  { "crush", "crushes" },           /* TYPE_CRUSH    */
  { "crush3", "crushes3" }           /* TYPE_CRUSH    */
};



/*
 * Used to remove invisibility; now it removes sanctuary
 */
void appear(struct char_data *ch) {
  if (affected_by_spell(ch, SPELL_SANCTUARY)) {
    affect_from_char(ch, SPELL_SANCTUARY);
    send_to_char("Your sanctuary is ended!\n\r", ch);
  }
  REMOVE_BIT(ch->specials.affected_by, AFF_SANCTUARY);
}


void load_messages(void) {

  FILE *f1;
  struct message_type *messages;
  int i, type;
  char chk[100];

  if (!(f1 = fopen(MESS_FILE, "r"))) {
    sprintf(buf2, "Error reading combat message file %s", MESS_FILE);
    perror(buf2);
    exit(0);
  }

  for (i = 0; i < MAX_MESSAGES; i++) {
    fight_messages[i].a_type = 0;
    fight_messages[i].number_of_attacks = 0;
    fight_messages[i].msg = 0;
  }

  fscanf(f1, " %s \n", chk);
  while (*chk == 'M') {
    fscanf(f1, " %d\n", &type);

    for (i = 0; (i < MAX_MESSAGES) && (fight_messages[i].a_type != type) &&
	   (fight_messages[i].a_type); i++);
    if (i >= MAX_MESSAGES) {
      log("SYSERR: Too many combat messages.");
      exit(0);
    }

    CREATE(messages, struct message_type, 1);
    fight_messages[i].number_of_attacks++;
    fight_messages[i].a_type = type;
    messages->next = fight_messages[i].msg;
    fight_messages[i].msg = messages;

    sprintf(buf2, "combat message #%d in file '%s'", i, MESS_FILE);

    /* Messages on kill */
    /* Read and color as hit */
    messages->die_msg.attacker_msg = fread_string(f1, buf2);
    sprintf(buf, "$CH%s", messages->die_msg.attacker_msg);
    free(messages->die_msg.attacker_msg);
    messages->die_msg.attacker_msg = strdup(buf);

    /* Read and color as damage */
    messages->die_msg.victim_msg = fread_string(f1, buf2);
    sprintf(buf, "$CD%s", messages->die_msg.victim_msg);
    free(messages->die_msg.victim_msg);
    messages->die_msg.victim_msg = strdup(buf);

    messages->die_msg.room_msg = fread_string(f1, buf2);

    /* Messages on miss--none have color */
    messages->miss_msg.attacker_msg = fread_string(f1, buf2);
    messages->miss_msg.victim_msg = fread_string(f1, buf2);
    messages->miss_msg.room_msg = fread_string(f1, buf2);

    /* Messages on hit */
    /* Attacker gets hit color */
    messages->hit_msg.attacker_msg = fread_string(f1, buf2);
    sprintf(buf, "$CH%s", messages->hit_msg.attacker_msg);
    free(messages->hit_msg.attacker_msg);
    messages->hit_msg.attacker_msg = strdup(buf);

    /* Victim gets damage color */
    messages->hit_msg.victim_msg = fread_string(f1, buf2);
    sprintf(buf, "$CD%s", messages->hit_msg.victim_msg);
    free(messages->hit_msg.victim_msg);
    messages->hit_msg.victim_msg = strdup(buf);

    messages->hit_msg.room_msg = fread_string(f1, buf2);

    /* Messages when one hits oneself */
    messages->self_msg.attacker_msg = fread_string(f1, buf2);

    /* Victim (which is attacker) gets damage message */
    messages->self_msg.victim_msg = fread_string(f1, buf2);
    sprintf(buf, "$CD%s", messages->self_msg.victim_msg);
    free(messages->self_msg.victim_msg);
    messages->self_msg.victim_msg = strdup(buf);

    messages->self_msg.room_msg = fread_string(f1, buf2);
    fscanf(f1, " %s \n", chk);
  }
  fclose(f1);
}



void update_pos(struct char_data *victim) {
  if (GET_HIT(victim) > 0) {
    if (victim->specials.fighting)
      GET_POS(victim) = POSITION_FIGHTING;
    else
      GET_POS(victim) = POSITION_STANDING;
  }
  else if (GET_HIT(victim) > 0 )
    GET_POS(victim) = POSITION_STANDING;
  else if (GET_HIT(victim) <= -GET_CON(victim)/2)
    GET_POS(victim) = POSITION_DEAD;
  else if (GET_HIT(victim) <= -GET_CON(victim)/4)
    GET_POS(victim) = POSITION_INCAP;
  else
    GET_POS(victim) = POSITION_STUNNED;
}



/* start one char fighting another (yes, it is horrible, I know... )  */
void set_fighting(struct char_data *ch, struct char_data *vict) {

  struct char_data *tmpch;

  if (ch == vict)
    return;

  if (ch->specials.fighting) {
    if (GET_POS(ch) > POSITION_STUNNED)
      GET_POS(ch) = POSITION_FIGHTING;
    return;
  }

  for (tmpch = combat_list; tmpch; tmpch = tmpch->next_fighting)
    if (tmpch == ch)
      break;

  if (!tmpch) {
    ch->next_fighting = combat_list;
    combat_list = ch;
  }

  ch->specials.fighting = vict;
  if (vict)
    GET_POS(ch) = POSITION_FIGHTING;

  stop_hiding(ch, TRUE);

 if (vict)
    stop_hiding(vict, FALSE);
}



/*
 * Remove a char from the list of fighting chars
 */
void stop_fighting(struct char_data *ch) {
  struct char_data *tmp;
  struct char_data *oppon;

  oppon = ch->specials.fighting;

  /*
   * Go through the combat list.  If a char is found that is fighting ch,
   * ch is not directly fighting that_char, and ch CAN_SEE that_char,
   * tmp = that_char, otherwise (if no characters are found) tmp = NULL.
   */
  for (tmp = combat_list; tmp; tmp = tmp->next_fighting)
    if (tmp->specials.fighting == ch && tmp != oppon
	&& CAN_SEE(ch, tmp))
      break;

  /*
   *If we didn't find a character above, and ch has not regenned energy or
   * has mental delay, and ch is alive (stunned or better), ch is no longer
   * fighting anyone, and we return.
   */
  if (!tmp && GET_ENERGY(ch) < ENE_TO_HIT || GET_MENTAL_DELAY(ch) &&
      GET_POS(ch) > POSITION_STUNNED) {
    ch->specials.fighting = 0;
    GET_POS(ch) = POSITION_STANDING;
    update_pos(ch);
    return;
    /* Do not remove from the list yet */
  }

  /* here remain only cases with Energy >= Ene_to_hit already, or dead ones */
  if (!tmp) {
    if (ch == combat_next_dude)
      combat_next_dude = ch->next_fighting;

    if (combat_list == ch)
      combat_list = ch->next_fighting;
    else {
      for (tmp = combat_list; tmp && (tmp->next_fighting != ch);
	   tmp = tmp->next_fighting);

      if (!tmp) {
	ch->next_fighting = 0;
	ch->specials.fighting = 0;
	if (GET_POS(ch) == POSITION_FIGHTING)
	  GET_POS(ch) = POSITION_STANDING;
	update_pos(ch);
	return; /* he's not fighting */
      }
      tmp->next_fighting = ch->next_fighting;
    }

    ch->next_fighting = 0;
    ch->specials.fighting = 0;
    if (GET_POS(ch) == POSITION_FIGHTING) {
      GET_POS(ch) = POSITION_STANDING;
      update_pos(ch);
    }
  }
  else {
    ch->specials.fighting = tmp;
    send_to_char("You turn to face your next enemy.\n\r", ch);
  }
}

void stop_fighting_him(struct char_data *ch) {

  struct char_data *tmp, *next_fighter;

  for (tmp = combat_list; tmp; tmp = next_fighter) {
    next_fighter = tmp->next_fighting;
    if (tmp->specials.fighting == ch) stop_fighting(tmp);
  }
}


/*
 * Checks to see if a hit breaks through a sanctuary
 * spell (or it didn't exist). Returns 0 if sanct remains
 */

int check_sanctuary(char_data * ch, char_data * victim) {

  affected_type *aff;
  int return_value;

  /* aff->modifier is the victim's original alignment */

  aff = affected_by_spell(victim, SPELL_SANCTUARY);
  if (!aff)
    return 1;

  /* The strength of the spell is weakened with every hit */

  if (GET_ALIGNMENT(ch) < 0)
    aff->modifier = aff->modifier - (number(0, (-1 * GET_ALIGNMENT(ch)) / 2));
  else
    aff->modifier = aff->modifier - (number(0, GET_ALIGNMENT(ch)));

  if (aff->modifier < 0) {
    affect_from_char(victim, SPELL_SANCTUARY);
    send_to_char("Your sanctuary is broken!\n\r\n\r", ch);
    return_value = 1;
    if (ch != victim) {
      act("You break $N's sanctuary!",FALSE, ch, 0, victim, TO_CHAR);
      act("$n breaks $N's sanctuary!",FALSE, ch, 0, victim, TO_ROOM);
    } else
      act("$n's sanctuary is broken!",FALSE, ch, 0, 0, TO_ROOM);
  } else {
    act("You could not break $S sanctity.",FALSE, ch, 0, victim, TO_CHAR);
    act("$n's aura flickers.",FALSE, victim, 0, 0, TO_ROOM);
    if (ch->specials.fighting == victim)
      stop_fighting(ch);
    return_value = 0;;
  }
  return return_value;
}


/*
 * This function gives to us our corpses "death_type"
 * effectively just returning a string.
 */
void
get_corpse_desc(struct obj_data *corpse, struct char_data *ch,
		int attacktype)
{
#define BUF_LEN 32

  char condition[BUF_LEN];

  switch (attacktype) {
  case 43: strncpy(condition, "rancid", BUF_LEN -1);
    break;
  case 25: strncpy(condition, "cleanly slain", BUF_LEN -1);
    break;
  case 71: strncpy(condition, "slightly charred", BUF_LEN -1);
    break;
  case 75: strncpy(condition, "slightly chilled", BUF_LEN -1);
    break;
  case 78: strncpy(condition, "electrocuted", BUF_LEN -1);
    break;
  case 81: strncpy(condition, "battered", BUF_LEN -1);
    break;
  case 84: strncpy(condition, "darkened", BUF_LEN -1);
    break;
  case 90: strncpy(condition, "burnt", BUF_LEN -1);
    break;
  case 91: strncpy(condition, "badly burnt", BUF_LEN -1);
    break;
  case 93: strncpy(condition, "frozen", BUF_LEN -1);
    break;
  case 96: strncpy(condition, "burnt", BUF_LEN -1);
    break;
  case 99: strncpy(condition, "almost unrecognisable", BUF_LEN -1);
    break;
  case 100: strncpy(condition, "harrowing", BUF_LEN -1);
    break;
  case 102: strncpy(condition, "agonised", BUF_LEN -1);
    break;
  case 103: strncpy(condition, "tormented", BUF_LEN -1);
    break;
  case 104: strncpy(condition, "shocked", BUF_LEN -1);
    break;
  case 105: strncpy(condition, "horribly darkened", BUF_LEN -1);
    break;
  case 106: strncpy(condition, "husk-like", BUF_LEN -1);
    break;
  case 107: strncpy(condition, "tortured", BUF_LEN -1);
    break;
  case 201: strncpy(condition, "badly burnt", BUF_LEN -1);
    break;
  case 130: strncpy(condition, "badly beaten", BUF_LEN -1);
    break;
  case 131: strncpy(condition, "crushed", BUF_LEN -1);
    break;
  case 132: strncpy(condition, "pierced", BUF_LEN -1);
    break;
  case 133: strncpy(condition, "slashed", BUF_LEN -1);
    break;
  case 134: strncpy(condition, "stabbed", BUF_LEN -1);
    break;
  case 135: strncpy(condition, "whipped", BUF_LEN -1);
    break;
  case 136: strncpy(condition, "skewered", BUF_LEN -1);
    break;
  case 137: strncpy(condition, "clawed", BUF_LEN -1);
    break;
  case 138: strncpy(condition, "bitten", BUF_LEN -1);
    break;
  case 139: strncpy(condition, "poisoned", BUF_LEN -1);
    break;
  case 140: strncpy(condition, "hacked", BUF_LEN -1);
    break;
  case 141: strncpy(condition, "disfigured", BUF_LEN -1);
    break;
  case 142: strncpy(condition, "crushed", BUF_LEN -1);
    break;
  case 143: strncpy(condition, "crushed", BUF_LEN -1);
    break;
  case 98: strncpy(condition, "darkened", BUF_LEN -1);
    break;
  default: strncpy(condition,  "silent", BUF_LEN -1);
  }
  if (world[ch->in_room].sector_type == SECT_WATER_NOSWIM ||
      world[ch->in_room].sector_type == SECT_WATER_SWIM ||
      world[ch->in_room].sector_type == SECT_UNDERWATER)
    asprintf(&(corpse->description), "The %s corpse of %s is floating here.",
	     condition,
	     IS_NPC(ch) ? GET_NAME(ch) :
	     pc_star_types[GET_RACE(ch)]);
  else
    asprintf(&(corpse->description), "The %s corpse of %s is lying here.",
	    condition,
	    IS_NPC(ch) ? GET_NAME(ch) :
	    pc_star_types[GET_RACE(ch)]);

  asprintf(&(corpse->short_description), "the %s corpse of %s", condition,
	   IS_NPC(ch)?GET_NAME(ch) :
	   pc_star_types[GET_RACE(ch)] );

}


/*
 * This fucntion creates and put money
 * into the corpse. It also prevents a gold
 * duplication loophole.
 * It also sets, after death, the characters
 * gold back to zero.
 * If the option passed in is zero we place the gold
 * in the object "object", if its not zero it all drops
 * on the ground.
 */
void
move_gold (struct char_data *ch, struct obj_data *object, int option)
{
  struct obj_data *create_money(int amount);
  struct obj_data *money;


  if (GET_GOLD(ch) > 0) {
    if (option == 0)
      if (IS_NPC(ch) || (!IS_NPC(ch) && ch->desc)) {
	money = create_money(GET_GOLD(ch));
	obj_to_obj(money, object);
      } else {
	money = create_money(GET_GOLD(ch));
	obj_to_room(money, ch->in_room);
      }
  }
  GET_GOLD(ch) = 0;
}


/*
 * This function determines how long the corpse will last
 * before it decays. I've added an extra int value called
 * duration here. This is just for possible future re-use
 * of the function. Might be nice to have varying decay
 * times in certain circumstances.
 */
void
corpse_decay_time (struct char_data *ch, struct obj_data *corpse, int duration)
{

  if (duration > 0)
    corpse->obj_flags.timer = duration;
  else
    if (IS_NPC(ch)) {
      if (MOB_FLAGGED(ch, MOB_ORC_FRIEND))
	corpse->obj_flags.timer = max_npc_corpse_time + 15;
      else
	corpse->obj_flags.timer = max_npc_corpse_time;
    } else
      corpse->obj_flags.timer = max_pc_corpse_time;

}

/*
 * This function gives a 4% random chance of an item disappearing
 * At most 1 item goes if your carrying under 25 items.
 * The first two objects that an NPC carries are immune to the
 * chance of being taken.
 */
void
remove_random_item (struct char_data *ch, struct obj_data *corpse)
{
  int chance, i;
  struct obj_data *obj;

  if (!IS_NPC(ch) || IS_AFFECTED(ch, AFF_CHARM) || IS_GUARDIAN(ch)) {
    chance = number(0, 24);
    i = 0;
    for (obj = corpse->contains;obj;obj = obj->next_content) {
      i++;

      if (i%25 == chance) {
        if (!IS_SET(obj->obj_flags.extra_flags, ITEM_NORENT) &&
	    GET_ITEM_TYPE(obj) != ITEM_KEY && !IS_ARTIFACT(obj)) {
          if (GET_ITEM_TYPE(obj) == ITEM_CONTAINER)
            i--;
          else {
            extract_obj(obj);
            break;
          }
        }
      }
    }
  }
}

void
remove_and_drop_object (struct char_data *ch)
{
  struct obj_data *obj;
  struct obj_data *next_obj;
  int i;

  for(obj = ch->carrying; obj; obj = next_obj){
    next_obj = obj->next_content;
    obj_to_room(obj, ch->in_room);
  }
  ch->carrying = 0;

  for (i = 0; i < MAX_WEAR; i++)
    if (ch->equipment[i])
      obj_to_room(unequip_char(ch, i), ch->in_room);
}

void
make_physical_corpse (struct char_data *ch, int attacktype)
{
  struct obj_data *obj;
  struct obj_data *corpse;
  int i;

  if(ch->player.corpse_num)
    corpse = read_object(ch->player.corpse_num, VIRT);
  else
    corpse = 0;

  if(!corpse) {
    CREATE(corpse, struct obj_data, 1);
    clear_object(corpse);
    corpse->next = object_list;
    object_list = corpse;
    corpse->item_number = NOWHERE;
    corpse->in_room = NOWHERE;
    corpse->name = str_dup("corpse");

    /* We call this function to retrieve our corpse "type"*/
    get_corpse_desc(corpse, ch, attacktype);
    corpse->obj_flags.type_flag = ITEM_CONTAINER;
    corpse->obj_flags.wear_flags = ITEM_TAKE;
    corpse->obj_flags.value[0] = 0; /* You can't store stuff in a corpse */
    corpse->obj_flags.value[3] = 1; /* corpse identifyer */
    corpse->obj_flags.weight = GET_WEIGHT(ch) + IS_CARRYING_W(ch);
    corpse->obj_flags.cost_per_day = 10000;
    corpse->obj_flags.level = GET_LEVEL(ch);
  }
  /*
   * Next two lines are used to identify to who the corpse belongs to
   * In the case of the first, its for the purposes of beheading
   * In the case of the second value, it allows the owner
   * of the corpse to freely use "get all corpse".
   */
  corpse->obj_flags.value[4] = IS_NPC(ch)?mob_index[ch->nr].virt:
    (-GET_IDNUM(ch));
  corpse->obj_flags.value[2] = IS_NPC(ch) ? mob_index[ch->nr].virt:
    -GET_IDNUM(ch);

  corpse->obj_flags.butcher_item = ch->specials.butcher_item;
  corpse->contains = ch->carrying;
  /* this function sorts out money related matters */
  move_gold(ch, corpse, 0);
  /*
   * Sets our corpse decay time
   * The third value passed in an integer value, if
   * this value is set to anything but 0 then that value
   * will become the corpse decay time.
   */
  corpse_decay_time(ch, corpse, 0);

  /* removing our victims eq */
  for (i = 0; i < MAX_WEAR; i++)
    if (ch->equipment[i])
      obj_to_obj(unequip_char(ch, i), corpse);


  ch->carrying = 0;
  IS_CARRYING_N(ch) = 0;
  IS_CARRYING_W(ch) = 0;

  /* putting everything into the corpse */
  for (obj = corpse->contains; obj; obj = obj->next_content)
    obj->in_obj = corpse;
  object_list_new_owner(corpse, 0);
  //remove_random_item(ch, corpse);
  /* moving the corpse into the room of our players death */
  obj_to_room(corpse, ch->in_room);

}

void
spirit_death (struct char_data *ch)
{
  act("$n vanishes into thin air.",TRUE, ch, 0, 0, TO_ROOM);
  remove_and_drop_object (ch);
  move_gold(ch, NULL, 1);

}

void
make_corpse(struct char_data *ch, int attacktype)
{

  if(!IS_SHADOW(ch))
    make_physical_corpse(ch, attacktype);
  else
    spirit_death(ch);
}



/* When ch kills victim */
void	change_alignment(struct char_data *ch, struct char_data *victim)
{
  int	align;
  extern int min_race_align[];
  extern int max_race_align[];

//    align = GET_ALIGNMENT(ch) - GET_ALIGNMENT(victim);

//    if((GET_ALIGNMENT(ch) > 0)&& (GET_ALIGNMENT(victim) < 0))
//      align = -GET_ALIGNMENT(ch) - GET_ALIGNMENT(victim);
//    if((GET_ALIGNMENT(ch) > 0)&& (GET_ALIGNMENT(victim) > 0))
//      align = -GET_ALIGNMENT(victim);

//       GET_ALIGNMENT(ch) += align/8;

//       if(GET_ALIGNMENT(ch) < -1000) GET_ALIGNMENT(ch) = -1000;

/*** current align system, commented out by Zenith
  align = (-GET_ALIGNMENT(victim) - GET_ALIGNMENT(ch))/3;

  if(IS_AGGR_TO(victim, ch) || MOB_FLAGGED(victim, MOB_AGGRESSIVE)){
    if((align < 0) && (align < - GET_ALIGNMENT(victim)*3/2))
      align = - GET_ALIGNMENT(victim)*3/2;
  }
  else{
    if((align < 0) && (align < - GET_ALIGNMENT(victim)*2 - 120))
      align = - GET_ALIGNMENT(victim)*2 - 120;
  }
  align = align *(GET_LEVEL(victim) + 2)/(GET_LEVEL(ch) + 10)/20;

  age = MOB_AGE_TICKS(victim, time(0)) * 40/(GET_LEVEL(victim)+20);

  if(IS_NPC(victim) && (GET_LEVEL(victim) > 5)){
    if((age < average_mob_life/2))
      align = align * (average_mob_life*60 + age*40)/(average_mob_life*100);
    else
      align = align * (150 - 50*average_mob_life/age)/100;

  }

****/
  if (ch == victim)
    return;  // -has- happened :)

   align = 0 ;// initialization, if nothing below is true, no gain :)

  if(GET_ALIGNMENT(victim) == 0)
    return;  //neutral mobs do not effect alignment

  // Supposedly "good" races
  if (RACE_GOOD(ch)){

    // Good vs evil: how things should be...
    if (GET_ALIGNMENT(victim) < 0 && GET_ALIGNMENT(ch) >= 0)
      align = -1 * GET_ALIGNMENT(victim) / 25;

    // Evil whities kill evil - recover slowly -- especially of mob non-agg
    if ((GET_ALIGNMENT(victim) < 0) && GET_ALIGNMENT(ch) < 0 )
      {
	if(MOB_FLAGGED(victim, MOB_AGGRESSIVE) || IS_AGGR_TO(victim, ch))
	  align = MAX(0, (GET_ALIGNMENT(ch)/2 - GET_ALIGNMENT(victim)) / 100);
	else
	  align = MAX(0, (GET_ALIGNMENT(ch)/2 - GET_ALIGNMENT(victim)) / 200);

      }
    // whities kill good - tut tut
    if (GET_ALIGNMENT(victim) > 0)
      if(MOB_FLAGGED(victim, MOB_AGGRESSIVE) || IS_AGGR_TO(victim, ch))
	align = 0 - GET_ALIGNMENT(victim) / 10;
      else
	align = 0 - GET_ALIGNMENT(victim) / 2;

  } else {

  // "evil" races

    if (GET_ALIGNMENT(victim) < GET_ALIGNMENT(ch))
      align = 1;
    else
      align = 0 - (GET_ALIGNMENT(victim ) / 25);
  }



  GET_ALIGNMENT(ch) += align;

  if(GET_ALIGNMENT(ch) < min_race_align[GET_RACE(ch)])
    GET_ALIGNMENT(ch) = min_race_align[GET_RACE(ch)];

  if(GET_ALIGNMENT(ch) > max_race_align[GET_RACE(ch)])
    GET_ALIGNMENT(ch) = max_race_align[GET_RACE(ch)];

}



void	death_cry(struct char_data *ch)
{
   int	door, was_in;

   /* Check to see if death cry is NULL. */
   if (ch->player.death_cry && strcmp(ch->player.death_cry, "(null)"))
   {
     strcpy(buf, ch->player.death_cry);
   }
   else
   {
     sprintf(buf,"Your blood freezes as you hear %s's death cry.",
	     (IS_NPC(ch))?"$n":GET_NAME(ch));
   }

   act(buf, FALSE, ch, 0, 0, TO_ROOM);
   was_in = ch->in_room;

   if (ch->player.death_cry2 && strcmp(ch->player.death_cry2, "(null)"))
   {
     strcpy(buf, ch->player.death_cry2);
   }
   else
   {
     sprintf(buf,"Your blood freezes as you hear %s's death cry.",
	     (IS_NPC(ch))?"someone":GET_NAME(ch));
   }
   for (door = 0; door < NUM_OF_DIRS; door++) {
      if (CAN_GO(ch, door))	 {
	 ch->in_room = world[was_in].dir_option[door]->to_room;
	 act(buf, FALSE, ch, 0, 0, TO_ROOM);
	 ch->in_room = was_in;
      }
   }
}



void
raw_kill(struct char_data *ch, int attacktype)
{
  waiting_type tmpwtl;

  if((ch->delay.wait_value >= 0) && (ch->delay.cmd > 0)){
    ch->delay.subcmd = -1;
    complete_delay(ch);
  }
  abort_delay(ch);

  if (ch->specials.fighting)
    stop_fighting(ch);

  if(special(ch, 0, "", SPECIAL_DEATH, &tmpwtl))
    return;

  if(IS_RIDING(ch))
    stop_riding(ch);

  if(IS_AFFECTED(ch, AFF_BASH))
    REMOVE_BIT(ch->specials.affected_by, AFF_BASH);

  while(ch->affected)
    affect_remove(ch, ch->affected);
  death_cry(ch);
  make_corpse(ch, attacktype);

  if(!IS_NPC(ch)) {
    save_char(ch, r_mortal_start_room[GET_RACE(ch)], 0);
    Crash_crashsave(ch);

    // Set his stats to 2/3 their current value.
    if (GET_STR(ch) > 0) SET_STR(ch, GET_STR(ch) * 2 / 3);
    if (GET_INT(ch) > 0) GET_INT(ch) = GET_INT(ch) * 2 / 3;
    if (GET_WILL(ch) > 0) GET_WILL(ch) = GET_WILL(ch) * 2/ 3;
    if (GET_DEX(ch) > 0) GET_DEX(ch) = GET_DEX(ch) * 2 / 3;
    if (GET_CON(ch) > 0) GET_CON(ch) = GET_CON(ch)  * 2 / 3;
    if (GET_LEA(ch) > 0) GET_LEA(ch) = GET_LEA(ch) * 2 / 3;

    // Set hits to 1, moves and mana to 0.
    GET_HIT(ch)  = 1;
    GET_MANA(ch) = 0;
    GET_MOVE(ch) = 0;

    if(GET_LEVEL(ch) < LEVEL_IMMORT) {
      ch->specials2.load_room = mortal_start_room[GET_RACE(ch)];
      extract_char(ch, r_mortal_start_room[GET_RACE(ch)]);
    }
    else{
      ch->specials2.load_room = immort_start_room;
      extract_char(ch, r_immort_start_room);
    }

    REMOVE_BIT(PLR_FLAGS(ch), PLR_WAS_KITTED);
  } else
    extract_char(ch);
}

/*
 * Simple enough function to check whether a character has
 * Death_ward up or not.
 */
int
check_death_ward(struct char_data *ch)
{
  if (affected_by_spell(ch, SPELL_DEATH_WARD) != NULL) {
    affect_from_char(ch, SPELL_DEATH_WARD);
    affect_total(ch);

    if (GET_HIT(ch) <= 0 && GET_HIT(ch) +
	affected_by_spell(ch, SPELL_DEATH_WARD)->modifier > 0) {
      act("$n's whole body flashes with blinding light, "
	  "and stirs back to life!",
	  FALSE, ch, 0, 0, TO_ROOM);
      send_to_char("Your ward flares up and sinks into your body."
		   "You are alive!\n\r", ch);

      if (GET_HIT(ch) < 0) {
	GET_HIT(ch) += affected_by_spell(ch, SPELL_DEATH_WARD)->modifier;
	if (!GET_HIT(ch))
	  GET_HIT(ch) = 1;
      }
      stop_fighting_him(ch);
      stop_fighting(ch);
      update_pos(ch);
      return TRUE;
    } else {
      send_to_char("Your ward sends one short pulse through your veins, "
                   "then fades.\r\n", ch);
      return FALSE;
    }
  }
  else
    return FALSE;
}

void
die(struct char_data *ch, struct char_data *killer, int attacktype)
{
  int poison_death;

  poison_death = 43;

  //  if (check_death_ward(ch))
  //return;

  /* Character doesn't die if call_trigger returns FALSE */
  if (call_trigger(ON_DIE, ch, killer, 0) == FALSE) {
    stop_fighting_him(ch);
    stop_fighting(ch);
    update_pos(ch);
    return;
  }

  /* the following piece is moved here, might cause problems... */
  if (killer) {
    if (IS_NPC(ch) || ch->desc)
      group_gain(killer, ch);

    if (!IS_NPC(ch)) {
      if (MOB_FLAGGED(killer, MOB_PET))
        vmudlog(BRF, "%s killed by %s (%s) at %s",
		GET_NAME(ch), GET_NAME(killer),
	        GET_NAME(killer->master), world[ch->in_room].name);
      else
        vmudlog(BRF, "%s killed by %s at %s",
		GET_NAME(ch),GET_NAME(killer),
                world[ch->in_room].name);
    }

    /*
     * If a mob kills a player, it no longer has memory of the player.
     *
     * Greeeeaaaat, so if a player dies to one mob with memory, every
     * other mob who remembers the player still has him on memory.  So
     * death isn't a clean start-over.
     */
    if (IS_NPC(killer) && !IS_NPC(ch) &&
	IS_SET(killer->specials2.act, MOB_MEMORY))
      forget(killer, ch);
  }

  /* A mob died.  Who cares. */
  if (IS_NPC(ch)) {
    raw_kill(ch, attacktype);
    return;
  }

  /* log mobdeaths */
  if (IS_NPC(killer))
    add_exploit_record(EXPLOIT_MOBDEATH, ch,
		       GET_IDNUM(killer), GET_NAME(killer));

  /* A player died: DT/poison/incap/etc. death. */
  if (!killer)
     gain_exp_regardless(ch, MIN(0, -(GET_EXP(ch)-3000)/ (GET_LEVEL(ch)+2)/10));

  else {
    if (attacktype == poison_death) {
	add_exploit_record(EXPLOIT_POISON, ch, 0, NULL);
	raw_kill(ch, attacktype);
	return;
    }
    pkill_create(ch);
    add_exploit_record(EXPLOIT_PK, ch, 0, NULL);  /* pk records to killers */

    /* add death records to dead player */
    /* Fingolfin: Jul 19: since we record mobdeaths earlier */
    if(killer && !IS_NPC(killer))
      add_exploit_record(EXPLOIT_DEATH, ch, 0, NULL);

    if(IS_NPC(killer) &&
       !(MOB_FLAGGED(killer, MOB_ORC_FRIEND) && MOB_FLAGGED(killer, MOB_PET)))
      gain_exp_regardless(ch, MIN(0,-(GET_EXP(ch)-3000)/(GET_LEVEL(ch)+2)));
    else
      if (attacktype == 43)
	add_exploit_record(EXPLOIT_POISON, ch, 0, NULL);
      gain_exp_regardless(ch, MIN(0,-(GET_EXP(ch)-3000)/(GET_LEVEL(ch)+2)/10));
    }

  GET_COND(ch, FULL) = 24;
  GET_COND(ch, THIRST) = 24;
  GET_COND(ch, DRUNK) = 0;

  raw_kill(ch, attacktype);
}



int
exp_with_modifiers(struct char_data *ch, struct char_data *victim,
		   int base_exp)
{
  int exp, age;

  /* Orcs get no experience for orc-friends */
  if (GET_RACE(ch) == RACE_ORC && IS_NPC(victim) &&
      MOB_FLAGGED(victim, MOB_ORC_FRIEND))
    return 0;

  base_exp /= MAX(GET_LEVEL(ch) + 1, GET_LEVEL(victim) - 2);

  if (!IS_NPC(victim))
    return base_exp;

  if (GET_LEVEL(victim) + 6 <  GET_LEVEL(ch))
    base_exp = 6 * base_exp / (GET_LEVEL(ch) - GET_LEVEL(victim));

  exp = base_exp;
  age = MOB_AGE_TICKS(victim, time(0)) * 40 / (GET_LEVEL(victim) + 20);

  if (GET_LEVEL(victim) > 5) {
    if (age < average_mob_life)
      exp = exp * (average_mob_life * 60 + age * 40) /
	(average_mob_life * 100);
    else
      exp = exp * (140 - 40 * average_mob_life / age) / 100;
  }

  if (MOB_FLAGGED(victim, MOB_AGGRESSIVE) ||
      IS_AGGR_TO(victim, ch))
    exp += base_exp / 5;

  if (MOB_FLAGGED(victim, MOB_FAST))
    exp += base_exp / 10;

  if (MOB_FLAGGED(victim, MOB_SWITCHING))
    exp += base_exp / 10;

  if (MOB_FLAGGED(victim, MOB_MEMORY))
    exp += base_exp / 20;

  if (victim->specials.default_pos < POSITION_STANDING)
    exp -= base_exp / 20;

  if (IS_GOOD(ch) && IS_GOOD(victim))
    exp = exp * 2/3;

  if (MOB_FLAGGED(victim, MOB_SPEC) && victim->nr >= 0 &&
      (mob_index[victim->nr].func || victim->specials.store_prog_number))
    exp += base_exp / 10;

  if (GET_DIFFICULTY(victim))
    exp = exp * GET_DIFFICULTY(victim) / 100;

  /* east exp bonus:  8 is the river */
  if (RACE_GOOD(ch) && zone_table[world[ch->in_room].zone].x > 8)
    exp += exp * MIN(zone_table[world[ch->in_room].zone].x - 8, 5) * 3/100;

  /* TEMPORARY: */
  exp += 2 * exp / MAX(1, GET_LEVEL(ch) - 1);

  return exp;

}



void
group_gain(struct char_data *ch, struct char_data *victim)
{
  int n_members, share, level_total, perc_total, spirit_gain, tmp;
  int group_bonus, npc_level_malus;
  struct char_data *k;
  struct char_data *t;

  if (ch->in_room == NOWHERE)
    return;
  if (ch->in_room != victim->in_room)
    return;

  k = ch->group_leader;
  if (k == NULL)
    k = ch;

  n_members = 0;
  level_total = 0;
  perc_total = 0;

  /* Find how many group members/levels and how much perception */
  for (t = world[ch->in_room].people; t; t = t->next_in_room)
    if (!IS_NPC(t) && (t->specials.fighting == victim ||
		       t->group_leader == k)) {
      n_members++;
      level_total += GET_LEVELB(t);
      perc_total += GET_PERCEPTION(t);
    }

  if (n_members >= 1) {
    share = GET_EXP(victim)/10;
    npc_level_malus = 0;

    if (IS_NPC(victim)) {
      spirit_gain = GET_LEVEL(victim) * get_naked_perception(victim);
      npc_level_malus = victim->specials.attacked_level;
      level_total += npc_level_malus;

      share =  share * (n_members + 1) / n_members;
    } else
      spirit_gain = GET_SPIRIT(victim)*100/4;

    share = share / level_total;
  } else {
    share = 0;
    return;
  }

  for (t = world[ch->in_room].people; t; t = t->next_in_room) {
    if (t != victim && GET_LEVEL(t) < LEVEL_IMMORT && !IS_NPC(t))
      if (t->specials.fighting == victim || t->group_leader == k) {
	if (perc_total)
	  tmp = spirit_gain * GET_PERCEPTION(t) / perc_total / 100;
	else
	  tmp = 0;

	if (tmp)
	  if (GET_ALIGNMENT(t) * GET_ALIGNMENT(victim) <= 0 ||
	      RACE_EVIL(t)) {
	    vsend_to_char(t, "Your spirit increases by %d.\n\r", tmp);
	    GET_SPIRIT(t) += tmp;
	  }

	group_bonus = MIN(share * GET_LEVELB(t) / 2,
			  (level_total - npc_level_malus - GET_LEVELB(t)) *
			  share / 4);
	tmp = exp_with_modifiers(t, victim,
				 share * GET_LEVELB(t) + group_bonus);

	vsend_to_char(t, "You receive your share of experience -- "
		      "%d points.\r\n", tmp);
	gain_exp(t, tmp);
	change_alignment(t, victim);

	/* save only 10% of the time to avoid lag in big groups */
	if (!number(0, 9))
	  save_char(t, NOWHERE, 1);
      }
  }
}



char replace_string_buf[500];

char *
replace_string(char *str, char *weapon_singular,
	       char *weapon_plural, char *bodypart)
{
  char *buf;
  char *cp;

  buf = replace_string_buf;
  cp = buf;

  for ( ; *str; str++) {
    if (*str == '#') {
      switch (*(++str)) {
      case 'W':
	for ( ; *weapon_plural; *(cp++) = *(weapon_plural++))
	  continue;
	break;
      case 'w':
	for ( ; *weapon_singular; *(cp++) = *(weapon_singular++))
	  continue;
	break;
      case 'b':
	if (bodypart[0]) {
	  *(cp++)=' ';
	  for ( ; *bodypart; *(cp++) = *(bodypart++))
	    continue;
	}
	break;
      case 's':
	if (bodypart[0]) {
	  *(cp++) = '\'';
	  *(cp++) = 's';
	}
	break;
      case 'r' :
	if (bodypart[0])
	  *(cp++)='r';
	break;
      default :
	*(cp++) = '#';
	break;
      }
    } else
      *(cp++) = *str;

    *cp = 0;
  }

  return buf;
}



/* use #w for singular (i.e. "slash") and #W for plural (i.e. "slashes") */
static struct dam_weapon_type {
  char *to_room;
  char *to_char;
  char *to_victim;
} dam_weapons[] = {
  { "$n misses $N#s#b with $s #w.",                     /* 0: 0     */
    "$CHYou miss $N#s#b with your #w.",
    "$CD$n misses you#r#b with $s #w." },
  { "$n scratches $N#s#b with $s #w.",                  /* 1: 1  */
    "$CHYou scratch $N#s#b as you #w $M.",
    "$CD$n scratches you#r#b as $e #W you." },
  { "$n barely #W $N#s#b.",                             /* 2: 2..3 */
    "$CHYou barely #w $N#s#b.",
    "$CD$n barely #W you#r#b." },
  { "$n lightly #W $N#s#b.",                            /* 3: 4..6  */
    "$CHYou lightly #w $N#s#b.",
    "$CD$n lightly #W you#r#b." },
  { "$n #W $N#s#b.",                                    /* 4: 7..11  */
    "$CHYou #w $N#s#b.",
    "$CD$n #W you#r#b." },
  { "$n #W $N#s#b hard.",                               /* 5: 12..17  */
    "$CHYou #w $N#s#b hard.",
    "$CD$n #W you#r#b hard." },
  { "$n #W $N#s#b very hard.",                          /* 6: 18..24  */
    "$CHYou #w $N#s#b very hard.",
    "$CD$n #W you#r#b very hard." },
  { "$n #W $N#s#b extremely hard.",                     /* 7: 25..33  */
    "$CHYou #w $N#s#b extremely hard.",
    "$CD$n #W you#r#b extremely hard." },
  { "$n deeply wounds $N#s#b with $s #w.",              /* 8: 34..60  */
    "$CHYou deeply wound $N#s#b with your #w.",
    "$CD$n deeply wounds you#r#b with $s #w." },
  { "$n severely wounds $N#s#b with $s #w!",            /* 9: 61..89 */
    "$CHYou severely wound $N#s#b with your #w!",
    "$CD$n severely wounds you#r#b with $s #w!" },
  { "$n MUTILATES $N#s#b with $s deadly #w!!",          /* 90: > 120  */
    "$CHYou MUTILATE $N#s#b with your deadly #w!!",
    "$CD$n MUTILATES you#r#b with $s deadly #w!!" }
};

void
dam_message(int dam, struct char_data *ch, struct char_data *victim,
	    int w_type, char *bodypart)
{
   struct obj_data *wield;
   char	*buf;
   int	msgnum;

   // That is, instead of using the TYPE_ which starts at 140+,
   // see spells.h for details, we use w_type range from 0 to 13 or so,
   // whatever number of different weapon types we have.
   w_type -= TYPE_HIT;   /* Change to base of table with text */

   wield = ch->equipment[WIELD];

   if (dam == 0)
     msgnum = 0;
   else if (dam <= 1)
     msgnum = 1;
   else if (dam <= 3)
     msgnum = 2;
   else if (dam <= 6)
     msgnum = 3;
   else if (dam <= 11)
     msgnum = 4;
   else if (dam <= 17)
     msgnum = 5;
   else if (dam <= 24)
     msgnum = 6;
   else if (dam <= 33)
     msgnum = 7;
   else if (dam <= 60)
     msgnum = 8;
   else	if (dam <= 90)
     msgnum = 9;
   else
     msgnum = 10;

   /* damage message to onlookers.  see note on replace_string */
   buf = replace_string(dam_weapons[msgnum].to_room,
			attack_hit_text[w_type].singular,
			attack_hit_text[w_type].plural,
			bodypart);
   act(buf, FALSE, ch, wield, victim, TO_NOTVICT,
       !msgnum ? TRUE : FALSE);

   /* damage message to damager */
   buf = replace_string(dam_weapons[msgnum].to_char,
			attack_hit_text[w_type].singular,
			attack_hit_text[w_type].plural,
			bodypart);
   act(buf, FALSE, ch, wield, victim, TO_CHAR,
       !msgnum ? TRUE : FALSE);

   /* damage message to damagee */
   buf = replace_string(dam_weapons[msgnum].to_victim,
			attack_hit_text[w_type].singular,
			attack_hit_text[w_type].plural,
			bodypart);
   act(buf, FALSE, ch, wield, victim, TO_VICT,
       !msgnum ? TRUE : FALSE);
}



void generate_damage_message(struct char_data *ch, struct char_data *victim, int dam, int attacktype, int hit_location)
{
	int nr;
	int i, j;
	struct message_type *messages;

	const race_bodypart_data& part = bodyparts[GET_BODYTYPE(victim)];
	char* body_part = part.parts[hit_location];
	if (IS_PHYSICAL(attacktype)) 
	{
		if (!ch->equipment[WIELD])
		{
			dam_message(dam, ch, victim, TYPE_HIT, body_part);
		}
		else
		{
			dam_message(dam, ch, victim, attacktype, body_part);
		}
	}
	else 
	{
		for (i = 0; i < MAX_MESSAGES; i++) 
		{
			if (fight_messages[i].a_type == attacktype) 
			{
				nr = dice(1, fight_messages[i].number_of_attacks);

				for (j = 1, messages = fight_messages[i].msg; j < nr && messages; j++)
				{
					messages = messages->next;
				}

				if (!messages) 
				{
					dam_message(dam, ch, victim, TYPE_HIT, body_part);
					vmudlog(BRF, "No fight_message for attacktype %d, i=%d\n", attacktype, i);
					break;
				}

				if (victim == ch) 
				{
					act(messages->self_msg.victim_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_CHAR);
					act(messages->self_msg.room_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_NOTVICT);
				}
				else if (dam != 0) 
				{
					if (GET_POS(victim) == POSITION_DEAD) 
					{
						act(messages->die_msg.attacker_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_CHAR);
						act(messages->die_msg.victim_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_VICT);
						act(messages->die_msg.room_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_NOTVICT);
					}
					else 
					{
						act(messages->hit_msg.attacker_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_CHAR);
						act(messages->hit_msg.victim_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_VICT);
						act(messages->hit_msg.room_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_NOTVICT);
					}
				}
				else { /* Dam == 0 */
					act(messages->miss_msg.attacker_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_CHAR);
					act(messages->miss_msg.victim_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_VICT);
					act(messages->miss_msg.room_msg, FALSE, ch, ch->equipment[WIELD], victim, TO_NOTVICT);
				}
			}
		}
	}
}



/*
 * damage now modified to return int - 1 if the victim was
 * killed, 0 if not.
 */
int
damage(struct char_data *ch, struct char_data *victim,
       int dam, int attacktype, int hit_location)
{
  struct affected_type *aff;
  int i, tmp, tmp1;
  struct waiting_type tmpwtl;
  extern void record_spell_damage(struct char_data *, struct char_data *,
				  int, int);

  if (!victim)
    return 0;

  if (!ch)
    ch = victim;  /* emergency fix */

  if (GET_POS(victim) <= POSITION_DEAD) {
    log("SYSERR: Attempt to damage a corpse.");
    return 0;   /* -je, 7/7/92 */
  }

  if (GET_POS(victim) <= POSITION_DEAD){
    send_to_char("Don't mess with the dead body!\r\n"
		 "Notify imps of this bug!\n\r", ch);
    return 0;
  }

  /*
   * Check hallucination.  Only check this if it's a physical hit type (to
   * avoid checking spells twice).
   */
  if (IS_PHYSICAL(attacktype) &&
      !check_hallucinate(ch, victim))
    return 0;

  /* Call special procs on damage */
  if (victim->specials.fighting != ch) {
    tmpwtl.targ1.ptr.ch = victim;
    tmpwtl.targ1.type = TARGET_CHAR;
    tmpwtl.targ2.ptr.other = 0;
    tmpwtl.targ2.type = TARGET_NONE;
    i = special(ch, 0, "", SPECIAL_DAMAGE, &tmpwtl);
    if (i) {
      if (ch->specials.fighting == victim)
	stop_fighting(ch);
      return 0;
    }
  }

  if (IS_SHADOW(victim)) {
    do_pass_through(ch, victim, 0);
    return 0;
  }

  if (!check_overkill(victim)) {
    act("There is too much activity to attack $N.",
	FALSE, ch, 0, victim, TO_CHAR);
    act("$N tried to attack you but could not get close enough.",
	FALSE, victim, 0, ch, TO_CHAR);
    act("$n tried to attack $N but could not get close enough.",
	FALSE, ch, 0, victim, TO_NOTVICT);
    return 0;
  }

  if (!check_sanctuary(ch, victim))
    return 0;

  /* Give the damager anger */
  if (victim && !IS_NPC(ch) && ch != victim) {
    struct affected_type af;
    struct affected_type *afptr;
    afptr = affected_by_spell(ch, SPELL_ANGER);
    if (afptr)
      afptr->duration = IS_NPC(victim) ? 2 : 5;
    else {
      af.type      = SPELL_ANGER;
      af.duration  = IS_NPC(victim) ? 2 : 5;
      af.modifier  = 0;
      af.location  = APPLY_NONE;
      af.bitvector = 0;
      affect_to_char(ch, &af);
    }
  }

  /* If sanctuary wasn't broken, victim is NULL */
  if (!victim) {
    set_fighting(ch, 0);
    return 0;
  }

  if (victim != ch) {
    if (GET_POS(ch) > POSITION_STUNNED) {
      if (!(ch->specials.fighting))
	set_fighting(ch, victim);

      if(IS_RIDING(ch) && (ch->mount_data.mount == victim))
	stop_riding(ch);
      if(IS_RIDING(victim) && (victim->mount_data.mount == ch))
	stop_riding(victim);

      if (IS_NPC(ch) && IS_NPC(victim) && victim->master &&
	  !number(0, 10) && IS_AFFECTED(victim, AFF_CHARM) &&
	  (victim->master->in_room == ch->in_room)) {
	if (ch->specials.fighting)
	  stop_fighting(ch);
	hit(ch, victim->master, TYPE_UNDEFINED);
	return 0;
      }
    }

    if (GET_POS(victim) > POSITION_STUNNED) {
      if (!(victim->specials.fighting))
	set_fighting(victim, ch);
      if (IS_NPC(victim) && IS_SET(victim->specials2.act, MOB_MEMORY) &&
	  !IS_NPC(ch) && (GET_LEVEL(ch) < LEVEL_IMMORT))
	remember(victim, ch);
      GET_POS(victim) = POSITION_FIGHTING;
    }
  }

  if (victim->master == ch)
    stop_follower(victim, FOLLOW_MOVE);
  if ((victim->group_leader == ch) && (victim != ch))
    stop_follower(victim, FOLLOW_GROUP);

  if (ch->group_leader && victim->group_leader == ch->group_leader &&
      victim != ch) {
    stop_follower(ch, FOLLOW_GROUP);
    stop_follower(victim, FOLLOW_GROUP);
  }

  if (IS_AFFECTED(ch, AFF_SANCTUARY))
    appear(ch);

  /*
   * XXX: This should really use stop_hiding.  But stop_hiding
   * needs modifications first.
   */
  if (IS_AFFECTED(ch, AFF_HIDE)) {
    act("$n steps out of $s cover.",TRUE, ch, 0, 0, TO_ROOM);
    send_to_char("You step out of your cover.\n\r",ch);
    REMOVE_BIT(ch->specials.affected_by, AFF_HIDE);
  }

  /* Remove delay if wait_wheel */
  if (dam > 0) {
    if (IS_AFFECTED(victim, AFF_WAITWHEEL) &&
	GET_WAIT_PRIORITY(victim) <= 40)
      break_spell(victim);
  }

  if (IS_NPC(victim) &&
      victim->specials.attacked_level < GET_LEVELB(ch))
    victim->specials.attacked_level = GET_LEVELB(ch);

  /* 33% chance to resist with protection physical*/
  tmp = check_resistances(victim, attacktype);
  if (number(0, 2) && IS_PHYSICAL(attacktype))
    tmp = 0;

  if (tmp > 0) {
    send_to_char("You resist a lot.\n\r", victim);
    act("$n resists a lot.\n\r",
	TRUE, victim, 0, 0, TO_ROOM);
    dam = dam * 2/3;
  }

  if (tmp < 0) {
    send_to_char("You feel it a lot.\n\r", victim);
    dam = dam * 3/2;
  }

  /* 10% chance for wild fighters to rush when they hit */
  if (IS_PHYSICAL(attacktype) && GET_SPEC(ch) == PLRSPEC_WILD
      && !number(0, 9)) {
    dam = dam * 3 / 2;

    send_to_char("You rush forward wildly.\n\r",ch);
    act("$n rushes forward wildly.", TRUE, ch, 0, 0, TO_ROOM);
  }

  if (dam > 230 && attacktype != SKILL_AMBUSH) {
    vmudlog(NRM, "Damage overflow for %s (%dhit).",
	    GET_NAME(ch), dam);
    dam = MIN(dam, 200);
  }
  dam = MAX(dam, 0);

  /*
   * Seether's shield:
   * If the victim has the shield spell up, we're going to take
   * some of his mana before his hitpoints, if he makes the roll.
   * It will only drain mana to 1 (not 0) and will do at -least-
   * 1 hp of damage; we allow for the hit to take at most all of
   * their mana minus one.
   *   if(affected_by_spell(victim, SPELL_SHIELD) &&
   *      attacktype != SKILL_AMBUSH) {
   *     if(number(1, 101) < (29 + GET_PROF_LEVEL(PROF_MAGE, victim) * 2)) {
   *       int shield_prof, mana_remove = 0;
   *
   *       shield_prof = number(1, GET_PROF_LEVEL(PROF_MAGE, victim)) +
   *                     number(1, GET_MANA(victim) / 3);
   *       mana_remove = MIN(MIN(dam - 1, shield_prof), GET_MANA(victim));
   *       GET_MANA(victim) -= mana_remove;
   *       dam -= mana_remove;
   *     }
   *   }
   *
   *
   * Fingol's shield:
   * We should compare the two - see how they work in the game.  If
   * the victim has shield spell up it absorbs up to 40% of damage
   * remaining by now (after other abs etc) at a cost of mana
   * dependent on mage level.  My simple version: 20 / mage_level
   * cost in mana.
   *
   * Dim's shield:
   *   mana cost = level * 20 / mage_level +
   *               number(0, mage_level) > i * 20 % mage_level ? 1 : 0;
   *
   * 01/01/00: now we check that attack is not bash, else damage will
   * be absorbed and the bash message won't be given.
   */
  if (affected_by_spell(victim, SPELL_SHIELD) &&
      attacktype != SKILL_AMBUSH && attacktype != SKILL_BASH) {
    i = (dam * 2 + 4) / 5;
    aff = affected_by_spell(victim, SPELL_SHIELD);

    /* cost of mana to absorb the hit */
    tmp1 = (i * 15) / GET_PROF_LEVEL(PROF_MAGE,victim);
    if (number(0, GET_PROF_LEVEL(PROF_MAGE, victim)) >
	i * 15 % GET_PROF_LEVEL(PROF_MAGE, victim))
      tmp1++;

    /* max hits for remaining mana */
    if (tmp1 > GET_MANA(victim)) {
      i = i * GET_MANA(victim) / tmp1;
      GET_MANA(victim) = 0;
    } else
      GET_MANA(victim) -= tmp1;
    dam -= i;
    if (GET_MANA(victim) == 0)
      affect_from_char(victim, SPELL_SHIELD);
    else
      if (aff->duration > 2)
	aff->duration = 2;
  }

  if (!call_trigger(ON_DAMAGE, victim, ch, 0)) {
    update_pos(victim);
    update_pos(ch);
    return 0;
  }

  /* Spell damage logging */
  record_spell_damage(ch, victim, attacktype, dam);

  GET_HIT(victim) -= dam;

  if (ch != victim)
    gain_exp(ch, (1 + GET_LEVEL(victim)) *
	     MIN(20 + GET_LEVEL(ch) * 2, dam) / (1 + GET_LEVEL(ch)));

  /* hack for PC vs. PC fights to skip the incap step */
  if (GET_HIT(victim) < 0 && !IS_NPC(ch))
    GET_HIT(victim) = MIN(GET_HIT(victim), -20);

  update_pos(victim);
  update_pos(ch);

  generate_damage_message(ch, victim, dam, attacktype, hit_location);

  if (dam > 0) {
    check_break_prep(victim);
    check_break_prep(ch);
  }

  /* Use send_to_char -- act() doesn't send message if you are DEAD. */
  switch (GET_POS(victim)) {
  case POSITION_INCAP:
    act("$n is incapacitated and will die, if not aided.",
	TRUE, victim, 0, 0, TO_ROOM);
    send_to_char("You are incapacitated and will slowly die, "
		 "if not aided.\n\r", victim);
    break;
  case POSITION_STUNNED:
    act("$n is stunned, but will probably regain consciousness again.",
	TRUE, victim, 0, 0, TO_ROOM);
    send_to_char("You're stunned, but will probably regain "
		 "consciousness again.\n\r", victim);
    break;
  case POSITION_DEAD:
    act("$n is dead!  R.I.P.", TRUE, victim, 0, 0, TO_ROOM);
    send_to_char("You are dead!  Sorry...\n\r", victim);
    break;
  default:  /* >= POSITION SLEEPING */
    /*
     * This code gets duplicated in hit().  Make sure to check
     * there if you change anything here.
     */
    if (dam > (GET_MAX_HIT(victim) / 5))
      act("That really did HURT!", FALSE, victim, 0, 0, TO_CHAR);

    if (GET_HIT(victim) < (GET_MAX_HIT(victim) / 5)) {
      act("You wish that your wounds would stop BLEEDING so much!",
	  FALSE, victim, 0, 0, TO_CHAR);
      if (IS_NPC(victim)) {
	if (IS_SET(victim->specials2.act, MOB_WIMPY))
	  if (GET_POSITION(victim) > POSITION_SLEEPING)
	    do_flee(victim, "", 0, 0, 0);
      }
    }

    if (!IS_NPC(victim) && WIMP_LEVEL(victim) && victim != ch &&
	GET_HIT(victim) < WIMP_LEVEL(victim))
      if (GET_POSITION(victim) > POSITION_SLEEPING &&
	  GET_TACTICS(victim) != TACTICS_BERSERK) {
	send_to_char("You wimp out, and attempt to flee!\n\r",
		     victim);
	do_flee(victim, "", 0, 0, 0);
      }
  }

  if (!IS_NPC(victim) && !(victim->desc && victim->desc->descriptor) &&
      (victim->specials.fighting) && GET_POS(victim) > POSITION_INCAP) {
    do_flee(victim, "", 0, 0, 0);
    victim->specials.was_in_room = victim->in_room;
  }

  if (!AWAKE(victim))
    if (victim->specials.fighting)
      stop_fighting(victim);

  if (GET_POS(victim) == POSITION_DEAD) {
    die(victim, ch, attacktype);
    return 1;
  } else
    return 0;
}



/*UPDATE* integreate parry message with other messages */
void
do_parry(struct char_data *ch, struct char_data *victim, int type)
{
  act("$N deflects $n's attack.",
      FALSE, ch, 0, victim, TO_NOTVICT, TRUE);
  act("$N deflects your attack.",
      FALSE, ch, 0, victim, TO_CHAR, TRUE);
  act("You deflect $n's attack.",
      FALSE, ch, 0, victim, TO_VICT, TRUE);
}



void
do_dodge(struct char_data *ch, struct char_data *victim, int type)
{
  act("$N dodges $n's attack.",
      FALSE, ch, 0, victim, TO_NOTVICT, TRUE);
  act("$N dodges your attack.",
      FALSE, ch, 0, victim, TO_CHAR, TRUE);
  act("You dodge $n's attack.",
      FALSE, ch, 0, victim, TO_VICT, TRUE);
}



void
do_evade(struct char_data *ch, struct char_data *victim, int type)
{
  act("$N distracts $n into missing $M.",
      FALSE, ch, 0, victim, TO_NOTVICT, TRUE);
  act("$N is not where you attack $M.",
      FALSE, ch, 0, victim, TO_CHAR, TRUE);
  act("You evade $n's attack.",
      FALSE, ch, 0, victim, TO_VICT, TRUE);
}



void
do_pass_through(struct char_data *ch, struct char_data *victim, int type)
{
  act("$n's attack passes clean through $N.",
      FALSE, ch, 0, victim, TO_NOTVICT, FALSE);
  act("Your attack passes clean through $N.",
      FALSE, ch, 0, victim, TO_CHAR, FALSE);
  act("$n's attack passes clean through you.",
      FALSE, ch, 0, victim, TO_VICT, FALSE);
}



void
do_riposte(struct char_data *ch, struct char_data *victim)
{
  act("$n's riposte catches $N off guard.",
      FALSE, ch, 0, victim, TO_NOTVICT, FALSE);
  act("Your riposte catches $N off guard.",
      FALSE, ch, 0, victim, TO_CHAR, FALSE);
  act("$n's riposte catches you off guard.",
      FALSE, ch, 0, victim, TO_VICT, FALSE);
}



int
weapon_hit_type(int weapon_type)
{
	int w_type;
	switch (weapon_type) {
	case 0:
	case 1:
	case 2:
		w_type = TYPE_WHIP; /* whip */
		break;
	case 3:
	case 4:
		w_type = TYPE_SLASH;
		break;
	case 5:
		w_type = TYPE_FLAIL;  /* flail */
		break;
	case 6:
		w_type = TYPE_CRUSH;
		break;
	case 7:
		w_type = TYPE_BLUDGEON;
		break;
	case 8:
	case 9:
		w_type = TYPE_CLEAVE;
		break;
	case 10:
		w_type = TYPE_SPEARS;
		break;
	case 11:
		w_type = TYPE_PIERCE;
		break;
	case 12:
		w_type = TYPE_SMITE;
		break;
	case 13:
		w_type = TYPE_WHIP;
	case 14:
		w_type = TYPE_BLUDGEON;
		break;
	default:
		w_type = TYPE_HIT;
		break;
	}
	return w_type;
}



/*
 * 'ch' is hitting 'victim'--does 'ch' find weakness?  Return
 * the correct amount of damage dealt after the check; if 'ch'
 * finds weakness, it will be more, otherwise it will be the
 * same as the amount of damage passed.
 */
int
check_find_weakness(struct char_data *ch, struct char_data *victim, int dam)
{
  int prob;

  if (!IS_NPC(ch)) {
    prob = GET_RAW_SKILL(ch, SKILL_EXTRA_DAMAGE) / 3 *
           GET_PROF_LEVEL(PROF_WARRIOR, ch) / 30;
    if (GET_PROF_LEVEL(PROF_WARRIOR, ch) > 30)
      prob += GET_PROF_LEVEL(PROF_WARRIOR, ch) - 30;

    if (prob > number(0, 99)) {
      dam += dam / 2;

      act("You discover a weakness in $N's defense!",
	  TRUE, ch, 0, victim, TO_CHAR);
      act("$n discovers a weakness in $N's defense!",
	  TRUE, ch, 0, victim, TO_NOTVICT);
      act("$n discovers a weakness in your defense!",
	  TRUE, ch, 0, victim, TO_VICT);
    }
  }

  return dam;
}



/*
 * Basically, if a character is wielding a one-handed weapon
 * with both hands, we cause them to lose energy, and thus slow
 * them down.
 *
 * NOTE: A weapon is considered one handed if it has a bulk of
 * 4 or less.
 */
void
check_grip(struct char_data *ch, struct obj_data *wielded)
{
  int bulk;

  if (wielded != NULL)
    bulk = wielded->obj_flags.value[2];
  else
    bulk = 0;

  if (bulk <= 4 && IS_AFFECTED(ch, AFF_TWOHANDED) && wielded)
    if (number(0, 99) < 30 + 5 - bulk) {
      GET_ENERGY(ch) -= 300 + ((5 - bulk) * 100);
      act("You struggle to maintain your grip on $p.",
	  FALSE, ch, wielded, NULL, TO_CHAR);
    }
}



/*
 * Assuming 'ch' is hitting 'victim', does 'ch' perform
 * an 'accurate' hit (via the accuracy skill)?
 *
 * Returns: 0 if the hit should not be accurate, 1 if the
 * hit should be accurate.
 */
int
check_accuracy(struct char_data *ch, struct char_data *victim)
{
  int prob;

  if (GET_TACTICS(ch) != TACTICS_CAREFUL &&
      GET_TACTICS(ch) != TACTICS_AGGRESSIVE)
    return 0;

  prob = GET_PROF_LEVEL(PROF_RANGER, ch);
  prob -= GET_SKILL_PENALTY(ch);
  prob -= GET_DODGE_PENALTY(ch);
  prob *= GET_SKILL(ch, SKILL_ACCURACY) / 100;
  if (number(0, 100) < prob) {
    act("You manage to find an opening in $N's defense!",
	TRUE, ch, NULL, victim, TO_CHAR);
    act("$n notices an opening in your defense!",
	TRUE, ch, NULL, victim, TO_VICT);
    return 1;
  }

  return 0;
}



/*
 * Hit was parried - can victim riposte?  Should encumbrance
 * be factored in?
 *
 * NOTE: riposte can only happen if an attack is deflected -
 * not dodged or evaded.
 */
int
check_riposte(struct char_data *ch, struct char_data *victim)
{
  int dam;
  int prob;
  struct obj_data *wielded;

  wielded = victim->equipment[WIELD];

  if (GET_SKILL(victim, SKILL_RIPOSTE) && !IS_NPC(victim) &&
      wielded && GET_POS(victim) == POSITION_FIGHTING &&
      !IS_SET(victim->specials.affected_by, AFF_BASH))
    if (wielded->obj_flags.value[2] <= 3) {
      prob = GET_SKILL(victim, SKILL_RIPOSTE);
      prob += GET_SKILL(victim, SKILL_STEALTH);
      prob += GET_DEX(victim) * 5;
      prob *= GET_PROF_LEVEL(PROF_RANGER, victim);
      prob /= 200;

      if (number(0, 99) <= prob) {
	do_riposte(victim, ch);
	dam = get_weapon_damage(wielded) *
	  MIN(GET_DEX(victim), 20) / number(50, 100);

	if (damage(victim, ch, dam,
		   weapon_hit_type(wielded->obj_flags.value[3]), 21))
	  return 1;
      }
    }

  return 0;
}



/*
 * Given a hit location and a damage, calculate how much damage
 * should be done after the victim's armor is factored in.  The
 * modified amount is returned.
 *
 * This used to be (tmp >= 0); this implied that torches
 * could parry. - Tuh
 */
int
armor_effect(struct char_data *ch, struct char_data *victim,
	     int damage, int location, int w_type)
{
  int base_damage;
  struct obj_data *armor;

  /* Bogus hit location */
  if (location < 0 || location > MAX_WEAR)
    return 0;

  /* If they've got armor, let's let it do its thing */
  if (victim->equipment[location] != NULL) {
    armor = victim->equipment[location];
    base_damage = damage;

    /* First, remove minimum absorb */
    damage -= armor->obj_flags.value[1];

    /* Spears hit the armor, but then go right through it */
    if (w_type != TYPE_SPEARS)
      damage -= (damage * armor_absorb(armor) + 50) / 100;
    else
      damage -= (damage * armor_absorb(armor) + 50) / 200;

    /*
     * Smiting weapons can sometimes crush an opponent's bones
     * under the armor; wearing armor will actually INCREASE
     * the amount of damage taken, since the bodypart cannot
     * recoil under the disfigured metal.  Additionally, one's
     * head can hit the armor from the inside and add extra
     * damage.
     *
     * Based on a real life program on the discovery channel.
     *
     * Alright, now smiting only works on rigid metal armor,
     * not chain or leather.  Unfortunately, it won't work on
     * armor made of mithril, even if the description says the
     * armor is made of mithril plats.  Perhaps mithril should
     * never appear in plates? Or perhaps we should have some
     * other flag to define rigidity? Or perhaps smiting should
     * not exist? :)
     */
    if (w_type == TYPE_SMITE && armor->obj_flags.material == 4)
      if (!number(0, 4)) {
	send_to_char("Your opponent's bones crunch loudly.\n\r",
		     ch);
	send_to_char("OUCH! You hear a crunching sound and feel "
		     "a sharp pain.\n\r", victim);
	send_to_room_except_two("You hear a crunching sound.\n\r",
				ch->in_room, ch, victim);
	damage = base_damage + (base_damage - damage);
      }
  }

  return damage;
}

//============================================================================
int get_evasion_malus(const char_data& attacker, const char_data& victim)
{
	if (!utils::is_affected_by(victim, AFF_EVASION))
		return 0;

	const int BASE_VALUE = 5;

	int defender_bonus = utils::get_prof_level(PROF_CLERIC, victim) / 2;
	int attacker_offset = utils::get_prof_level(PROF_CLERIC, attacker) * (100 - utils::get_perception(attacker)) / 200;

	if (utils::is_npc(attacker))
	{
		attacker_offset /= 2;
	}

	// Always return at least the base value.
	if (attacker_offset > defender_bonus)
		return BASE_VALUE;

	return BASE_VALUE + defender_bonus - attacker_offset;
}

//============================================================================
void hit(struct char_data *ch, struct char_data *victim, int type)
{
  struct obj_data *wielded = 0; /* weapon that ch wields */
  int hit_accurate;         /* has accuracy roll been made? */
  int w_type; /* weapon type, like TYPE_SLASH */
  int OB;
  int dam = 0, dodge_malus, evasion_malus;
  int location;
  int tmp; /* rolled number stored as the chance to hit, adds OB */
  struct waiting_type tmpwtl;
  extern struct race_bodypart_data bodyparts[15];

  if (ch->in_room != victim->in_room) {
    log("SYSERR: NOT SAME ROOM WHEN FIGHTING!");
    return;
  }

  if (GET_POS(ch) < POSITION_FIGHTING)
    return;

  hit_accurate = check_accuracy(ch, victim);

  if (ch->equipment[WIELD] &&
      ch->equipment[WIELD]->obj_flags.type_flag == ITEM_WEAPON) {
    wielded = ch->equipment[WIELD];
    w_type = weapon_hit_type(wielded->obj_flags.value[3]);
    dam = get_weapon_damage(wielded);
  } else {
    /* XXX: Can we find some uses of attack_type elsewhere? */
    if (IS_NPC(ch) && ch->specials.attack_type >= TYPE_HIT)
      w_type = ch->specials.attack_type;
    else {
      w_type = TYPE_HIT;
      if (!IS_NPC(ch))
	dam = BAREHANDED_DAMAGE * 10;
    }
  }

  /*
   * Calculate hits/misses/damage
   *
   * NOTE: If tmp is 35, then the hit will succeed no matter
   * what.  This is not just because of the 100 OB bonus, but
   * because of the if statements which decide whether a dodge,
   * parry or evade is possible.
   */
  tmp = number(1, 35);
  OB = get_real_OB(ch);
  OB += number(1, 55 + OB/4);
  OB += tmp;
  OB -= OB/8;
  OB -= 40;
  if (tmp == 35)
    OB += 100;

  dodge_malus = evasion_malus = 0;
  if (!hit_accurate) {
	evasion_malus = get_evasion_malus(*ch, *victim);
    dodge_malus = get_real_dodge(victim) + evasion_malus;
    OB -= dodge_malus;
  }

  if (GET_POS(victim) < POSITION_FIGHTING)
    OB += 10 * (POSITION_FIGHTING - GET_POS(victim));

  /* checking here for the hit legitimacy... i hate this */
  if (GET_ENERGY(ch) >= ENE_TO_HIT) {
    GET_ENERGY(ch) -= ENE_TO_HIT;

    if (IS_SHADOW(victim)) {
      if (!weapon_willpower_damage(ch, victim))
	do_pass_through(ch, victim, 0);
      return;
    }

    if (OB < 0 && tmp != 35) {
      if (number(0, dodge_malus) < evasion_malus)
	do_evade(ch, victim, w_type);
      else
	do_dodge(ch, victim, w_type);
    } else {
      if (GET_POS(victim) > POSITION_STUNNED)
	OB -= get_real_parry(victim) * GET_CURRENT_PARRY(victim) / 100;

      SET_CURRENT_PARRY(victim) = GET_CURRENT_PARRY(victim) * 2/3;

      if (tmp == 35)
	OB = MAX(OB, 0);

      if (OB < 0) {
	do_parry(ch, victim, w_type);
	if (check_riposte(ch, victim))
	  return;

	check_grip(ch, wielded);
	check_grip(victim, victim->equipment[WIELD]);
      } else {
	/* Generate a hit location based on bodypart structure */
	/*
	 *XXX: Make into a separate function that treats bodyparts
	 * for all body types well.
	 */
	location = 0;
	if (bodyparts[GET_BODYTYPE(victim)].bodyparts) {
	  tmp = number(1, 100);

	  for ( ; tmp > 0 && location < MAX_BODYPARTS; location++)
	    tmp -= bodyparts[GET_BODYTYPE(victim)].percent[location];
	}

	if (location)
	  location--;
	if (IS_NPC(ch))
	  dam /= 2; /* mobs have weapon damage halved */

	/*
	 * 100 str would double damage, double effect for
	 * 2-handers, last factor is avg. 1.4 with str 13
	 * OB bonus factored in, and mult. by number between
	 * 1 and 2, with  numbers close to 1 more probable
	 */
	dam += GET_DAMAGE(ch) * 10;
	tmp = number(0, 100);
	if (hit_accurate)
	  dam = (dam * (10000 + (tmp*tmp))) / 100000;
	else  /* damage divided again by 10 */
	  dam = (dam * (OB + 100) * (10000 + (tmp * tmp) +
				     (IS_TWOHANDED(ch) ? 2 : 1) * 133 *
				     GET_BAL_STR(ch))) / 13300000;

	dam = check_find_weakness(ch, victim, dam);

	tmp = bodyparts[GET_BODYTYPE(victim)].armor_location[location];
	dam = armor_effect(ch, victim, dam, tmp, w_type);

	if (GET_POS(victim) < POSITION_FIGHTING)
	  dam += dam/2;

	dam = MAX(0, dam);  /* Not less than 0 damage */
	damage(ch, victim, dam, w_type, location);

	if (dam > 0)
	  check_grip(ch, wielded);

	/* so we won't check engagement for hit people */
	return;
      }
    }
  }

  /*
   * Following moved here from damage function, so dodge/parry
   * would engage.  It might be better done by just rewriting
   * dodge to go through damage parser, we'll see.
   *
   * Later: I think it could be marginally better to send this
   * code through the damage function, mainly so that we don't
   * have to worry about duplicated code.  However, we might
   * not have a very elegant way to preclude parries/dodges
   * from calling the SPECIAL_DAMAGE proc in that case.
   *
   * NOTE: Everything beyond this point happens ONLY if 'ch'
   * didn't have enough energy to hit, or 'ch' was parried,
   * dodged or evaded.  If any amount of damage is dealt
   * (whether it be by riposte, a normal hit, etc), this code
   * is not reached, and its functionality is duplicated in
   * damage().  It is for this reason why I think these calls
   * for SPECIAL_DAMAGE procs are highly out of place, and
   * this mess of position updating is also absolutely horrid.
   */
  if(ch->specials.fighting != victim) {
    tmpwtl.targ1.ptr.ch = victim;
    tmpwtl.targ1.type = TARGET_CHAR;
    tmpwtl.targ2.ptr.other = 0;
    tmpwtl.targ2.type = TARGET_NONE;
    tmp = special(ch, 0, "", SPECIAL_DAMAGE, &tmpwtl);
    if(tmp) {
      if(ch->specials.fighting == victim)
	stop_fighting(ch);
      return;
    }
  }

  if (GET_POS(ch) > POSITION_STUNNED && victim != ch)
    if (!ch->specials.fighting || GET_POS(ch) != POSITION_FIGHTING)
      set_fighting(ch, victim);

  if (GET_POS(victim) > POSITION_STUNNED) {
    if (!victim->specials.fighting ||
       GET_POS(victim) != POSITION_FIGHTING)
      set_fighting(victim, ch);

    if(IS_NPC(victim) && IS_SET(victim->specials2.act, MOB_MEMORY) &&
       !IS_NPC(ch) && (GET_LEVEL(ch) < LEVEL_IMMORT))
      remember(victim, ch);

    GET_POS(victim) = POSITION_FIGHTING;
  }

  if (GET_HIT(victim) < GET_MAX_HIT(victim) / 5) {
    act("You wish that your wounds would stop BLEEDING so much!",
	FALSE, victim, 0, 0, TO_CHAR);
    if (IS_NPC(victim))
      if (IS_SET(victim->specials2.act, MOB_WIMPY))
	if (GET_POSITION(victim) > POSITION_SLEEPING)
	  do_flee(victim, "", 0, 0, 0);
  }

  if (!IS_NPC(victim) && WIMP_LEVEL(victim) && victim != ch &&
      GET_HIT(victim) < WIMP_LEVEL(victim)) {
    if (GET_POSITION(victim) > POSITION_SLEEPING) {
      send_to_char("You wimp out, and attempt to flee!\n\r", victim);
      do_flee(victim, "", 0, 0, 0);
    }
  }

  if (!IS_NPC(victim) && !(victim->desc && victim->desc->descriptor) &&
      victim->specials.fighting && GET_POS(victim)> POSITION_INCAP) {
    do_flee(victim, "", 0, 0, 0);
    victim->specials.was_in_room = victim->in_room;
  }
}



/*
 * Control all of the fights going on; works on PULSE_VIOLENCE
 */
void
perform_violence(int mini_tics)
{
  struct char_data *ch;

  for(ch = combat_list; ch; ch = combat_next_dude) {
    combat_next_dude = ch->next_fighting;

    SET_CURRENT_PARRY(ch) = MIN(GET_CURRENT_PARRY(ch) + 3, 100);

    if(GET_MENTAL_DELAY(ch) && (GET_MENTAL_DELAY(ch) > -120))
      GET_MENTAL_DELAY(ch)--;

    if(GET_MENTAL_DELAY(ch) > 1)
      continue;

    if(ch->specials.fighting &&
       (IS_MENTAL(ch) || (IS_NPC(ch) && IS_SHADOW(ch->specials.fighting)))) {
      if((GET_POS(ch) >= POSITION_FIGHTING) && !IS_AFFECTED(ch, AFF_WAITING))
	do_mental(ch, "", 0, 0, 0);
      continue; // have in mind, the ch could die himself
    }


    if(!IS_AFFECTED(ch, AFF_WAITING) || (GET_WAIT_PRIORITY(ch) == 29) ||
       (GET_WAIT_PRIORITY(ch) == 59)) {
      if((GET_POS(ch) >= POSITION_FIGHTING) &&
	 (GET_ENERGY(ch) <= ENE_TO_HIT))
	GET_ENERGY(ch) += GET_ENE_REGEN(ch);
      else if(IS_NPC(ch) && !ch->delay.wait_value)
	do_stand(ch,"",0,0,0);

      if((GET_ENERGY(ch) > ENE_TO_HIT)) {
	if(AWAKE(ch) && ch->specials.fighting &&
	   (ch->in_room == ch->specials.fighting->in_room))
	  hit(ch, ch->specials.fighting, TYPE_UNDEFINED);
	else /* Not in same room */
	  stop_fighting(ch);
      }
    }
  }
}



#define SWORD_HAND WIELD
#define SHIELD_HAND WEAR_SHIELD

ACMD(do_twohand)
{
  if(!ch->equipment[SWORD_HAND]) {
    send_to_char("You are not wielding anything.\n\r",ch);
    return;
  }

  if(subcmd == SCMD_TWOHANDED) {
    if(IS_TWOHANDED(ch)) {
      send_to_char("You are already wielding your weapon with two hands.\n\r",
		   ch);
      return;
    }

    if(ch->equipment[SHIELD_HAND]) {
      send_to_char("You can not wield a weapon in two hands while "
		   "wearing a shield.\n\r", ch);
      return;
    }

    if((ch->equipment[SWORD_HAND])->obj_flags.value[2] <= 4)
      send_to_char("This weapon seems too small to be used two-handed.\n\r",
		   ch);

    SET_BIT(ch->specials.affected_by, AFF_TWOHANDED);
    send_to_char("You wield your weapon with both hands.\n\r", ch);
    act("$n takes $s weapon with both hands.", FALSE, ch, 0, 0, TO_ROOM);
    affect_total(ch);
    return;
  }

  if(subcmd == SCMD_ONEHANDED) {
    if(!IS_TWOHANDED(ch)) {
      send_to_char("You are already wielding your weapon with one hand.\n\r",
		   ch);
      return;
    }
    REMOVE_BIT(ch->specials.affected_by, AFF_TWOHANDED);
    send_to_char("You wield your weapon with one hand.\n\r",ch);
    act("$n takes $s weapon with one hand.",FALSE,ch,0,0,TO_ROOM);
    affect_total(ch);
    return;
  }
}



int
check_overkill(struct char_data *ch)
{
  struct char_data *tmpch;
  int num_fighting = 0;

  if(!ch || IS_NPC(ch))
    return 1;

  if(GET_BODYTYPE(ch) == 1)
    for(tmpch = combat_list; tmpch; tmpch = tmpch->next_fighting)
      if(tmpch->specials.fighting == ch)
	if(!(IS_NPC(tmpch)))
	  num_fighting++;

  if(num_fighting > 3 && number(0, num_fighting))
    return 0;

  return 1;
}



int
check_hallucinate(struct char_data *ch, struct char_data *victim)
{
  struct affected_type *aff;

  /*
   * Check for a hallucinating ch and check whether or not
   * they just hit thin air.  If they did, remove a modifier.
   * If not, remove the effect.  If the modifier is now 0 as
   * a result of removal, remove the effect.
   */
  if(IS_AFFECTED(ch, AFF_HALLUCINATE)) {
    for(aff = ch->affected; aff; aff = aff->next) {
      if(aff->type == SPELL_HALLUCINATE) {
        /* We found the affection, test the modifier */
        if(number(1, 100) > (100 / (aff->modifier + 1))) {
          /* They hit thin air.  Subtract a number from the modifier */
          aff->modifier--;

          act("You hit thin air!", FALSE, ch, 0, victim, TO_CHAR);
          act("$N pauses in confusion as they hit thin air!",
	      FALSE, victim, 0, ch, TO_CHAR);
          act("$n pauses in confusion as they hit thin air!",
	      FALSE, ch, 0, victim, TO_NOTVICT);

          /* If the modifier is now 0, unaffect the character */
          if(aff->modifier == 0)
            affect_from_char(ch, SPELL_HALLUCINATE);

          /* Get out of the damage sequence, because we did miss, in effect */
          return 0;
        }
        else {
          /*
	   * They hit the player.  Unaffect the character and
	   * continue the damage sequence as though nothing
	   * happened.
	   */
          affect_from_char(ch, SPELL_HALLUCINATE);
          return 1;
        }
      }
    }
  }

  /* No affection, so return 1 (indicating that ch is not affected) */
  return 1;
}
