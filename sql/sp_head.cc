/* Copyright (C) 2002 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifdef __GNUC__
#pragma implementation
#endif

#include "mysql_priv.h"
#include "sp_head.h"
#include "sp.h"
#include "sp_pcontext.h"
#include "sp_rcontext.h"

/* Evaluate a (presumed) func item. Always returns an item, the parameter
** if nothing else.
*/
static Item *
eval_func_item(THD *thd, Item *it, enum enum_field_types type)
{
  it= it->this_item();

  if (it->fix_fields(thd, 0, NULL))
    return it;			// Shouldn't happen?

  /* QQ How do we do this? Is there some better way? */
  switch (type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
    it= new Item_int(it->val_int());
    break;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    it= new Item_real(it->val());
    break;
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_NEWDATE:
    {
      char buffer[MAX_FIELD_WIDTH];
      String tmp(buffer, sizeof(buffer), default_charset_info);
      String *s= it->val_str(&tmp);

      it= new Item_string(s->c_ptr_quick(), s->length(), default_charset_info);
      break;
    }
  case MYSQL_TYPE_NULL:
    it= new Item_null();	// A NULL is a NULL is a NULL...
    break;
  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_GEOMETRY:
    /* QQ Don't know what to do with the rest. */
    break;
  }

  return it;
}

sp_head::sp_head(LEX_STRING *name, LEX *lex)
  : m_simple_case(FALSE)
{
  const char *dstr = (const char*)lex->buf;

  m_call_lex= lex;
  m_name= new Item_string(name->str, name->length, default_charset_info);
  m_defstr= new Item_string(dstr, lex->end_of_query - lex->buf,
			    default_charset_info);
  my_init_dynamic_array(&m_instr, sizeof(sp_instr *), 16, 8);
  m_backpatch.empty();
}

int
sp_head::create(THD *thd)
{
  String *name= m_name->const_string();
  String *def= m_defstr->const_string();

  return sp_create_procedure(thd,
			     name->c_ptr(), name->length(),
			     def->c_ptr(), def->length());
}

int
sp_head::execute(THD *thd)
{
  int ret= 0;
  sp_instr *p;
  sp_pcontext *pctx = m_call_lex->spcont;
  uint csize = pctx->max_framesize();
  uint params = pctx->params();
  sp_rcontext *octx = thd->spcont;
  sp_rcontext *nctx = NULL;
  my_bool tmp_octx = FALSE;	// True if we have allocated a temporary octx

  if (csize > 0)
  {
    uint i;
    List_iterator_fast<Item> li(m_call_lex->value_list);
    Item *it = li++;		// Skip first one, it's the procedure name

    nctx = new sp_rcontext(csize);
    if (! octx)
    {				// Create a temporary old context
      octx = new sp_rcontext(csize);
      tmp_octx = TRUE;
    }
    // QQ: No error checking whatsoever right now. Should do type checking?
    for (i = 0 ; (it= li++) && i < params ; i++)
    {
      sp_pvar_t *pvar = pctx->find_pvar(i);

      if (! pvar)
	nctx->set_oindex(i, -1); // Shouldn't happen
      else
      {
	if (pvar->mode == sp_param_out)
	  nctx->push_item(it->this_item()); // OUT
	else
	  nctx->push_item(eval_func_item(thd, it, pvar->type)); // IN or INOUT
	// Note: If it's OUT or INOUT, it must be a variable.
	// QQ: Need to handle "global" user/host variables too!!!
	if (pvar->mode == sp_param_in)
	  nctx->set_oindex(i, -1); // IN
	else			// OUT or INOUT
	  nctx->set_oindex(i, static_cast<Item_splocal *>(it)->get_offset());
      }
    }
    // The rest of the frame are local variables which are all IN.
    // QQ We haven't found any hint of what the value is when unassigned,
    //    so we set it to NULL for now. It's an error to refer to an
    //    unassigned variable anyway (which should be detected by the parser).
    for (; i < csize ; i++)
      nctx->push_item(NULL);
    thd->spcont= nctx;
  }

  {				// Execute instructions...
    uint ip= 0;

    while (ret == 0)
    {
      sp_instr *i;

      i = get_instr(ip);	// Returns NULL when we're done.
      if (i == NULL)
	break;
      ret= i->execute(thd, &ip);
    }
  }

  // Don't copy back OUT values if we got an error
  if (ret == 0 && csize > 0)
  {
    List_iterator_fast<Item> li(m_call_lex->value_list);
    Item *it = li++;		// Skip first one, it's the procedure name

    // Copy back all OUT or INOUT values to the previous frame, or
    // set global user variables
    for (uint i = 0 ; (it= li++) && i < params ; i++)
    {
      int oi = nctx->get_oindex(i);

      if (oi >= 0)
      {
	if (! tmp_octx)
	  octx->set_item(nctx->get_oindex(i), nctx->get_item(i));
	else
	{			// A global user variable
#if 0
	  // QQ This works if the parameter really is a user variable, but
	  // for the moment we can't assure that, so it will crash if it's
	  // something else. So for now, we just do nothing, to avoid a crash.
	  // Note: This also assumes we have a get_name() method in
	  //       the Item_func_get_user_var class.
	  Item *item= nctx->get_item(i);
	  Item_func_set_user_var *suv;
	  Item_func_get_user_var *guv= static_cast<Item_func_get_user_var*>(it);

	  suv= new Item_func_set_user_var(guv->get_name(), item);
	  suv->fix_fields(thd, NULL, &item);
	  suv->fix_length_and_dec();
	  suv->update();
#endif
	}
      }
    }

    if (tmp_octx)
      thd->spcont= NULL;
    else
      thd->spcont= octx;
  }

  return ret;
}


// Reset lex during parsing, before we parse a sub statement.
void
sp_head::reset_lex(THD *thd)
{
  memcpy(&m_lex, &thd->lex, sizeof(LEX)); // Save old one
  /* Reset most stuff. The length arguments doesn't matter here. */
  lex_start(thd, m_lex.buf, m_lex.end_of_query - m_lex.ptr);
  /* We must reset ptr and end_of_query again */
  thd->lex.ptr= m_lex.ptr;
  thd->lex.end_of_query= m_lex.end_of_query;
  /* And keep the SP stuff too */
  thd->lex.sphead = m_lex.sphead;
  thd->lex.spcont = m_lex.spcont;
  /* Clear all lists. (QQ Why isn't this reset by lex_start()?).
     We may be overdoing this, but we know for sure that value_list must
     be cleared at least. */
  thd->lex.col_list.empty();
  thd->lex.ref_list.empty();
  thd->lex.drop_list.empty();
  thd->lex.alter_list.empty();
  thd->lex.interval_list.empty();
  thd->lex.users_list.empty();
  thd->lex.columns.empty();
  thd->lex.key_list.empty();
  thd->lex.create_list.empty();
  thd->lex.insert_list= NULL;
  thd->lex.field_list.empty();
  thd->lex.value_list.empty();
  thd->lex.many_values.empty();
  thd->lex.var_list.empty();
  thd->lex.param_list.empty();
  thd->lex.proc_list.empty();
  thd->lex.auxilliary_table_list.empty();
}

// Restore lex during parsing, after we have parsed a sub statement.
void
sp_head::restore_lex(THD *thd)
{
  // Update some state in the old one first
  m_lex.ptr= thd->lex.ptr;
  m_lex.next_state= thd->lex.next_state;

  // Collect some data from the sub statement lex.
  if (thd->lex.sql_command == SQLCOM_CALL)
  {
    // We know they are Item_strings (since we put them there ourselves)
    // It would be slightly faster to keep the list sorted, but we need
    // an "insert before" method to do that.
    Item_string *proc= static_cast<Item_string*>(thd->lex.value_list.head());
    String *snew= proc->val_str(NULL);
    List_iterator_fast<Item_string> li(m_calls);
    Item_string *it;

    while ((it= li++))
    {
      String *sold= it->val_str(NULL);

      if (stringcmp(snew, sold) == 0)
	break;
    }
    if (! it)
      m_calls.push_back(proc);

  }
  // Merge used tables
  // QQ ...or just open tables in thd->open_tables?
  //    This is not entirerly clear at the moment, but for now, we collect
  //    tables here.
  for (SELECT_LEX *sl= thd->lex.all_selects_list ;
       sl ;
       sl= sl->next_select())
  {
    for (TABLE_LIST *tables= sl->get_table_list() ;
	 tables ;
	 tables= tables->next)
    {
      List_iterator_fast<char *> li(m_tables);
      char **tb;

      while ((tb= li++))
	if (strcasecmp(tables->real_name, *tb) == 0)
	  break;
      if (! tb)
	m_tables.push_back(&tables->real_name);
    }
  }

  memcpy(&thd->lex, &m_lex, sizeof(LEX)); // Restore lex
}

void
sp_head::push_backpatch(sp_instr *i, sp_label_t *lab)
{
  bp_t *bp= (bp_t *)my_malloc(sizeof(bp_t), MYF(MY_WME));

  if (bp)
  {
    bp->lab= lab;
    bp->instr= i;
    (void)m_backpatch.push_front(bp);
  }
}

void
sp_head::backpatch(sp_label_t *lab)
{
  bp_t *bp;
  uint dest= instructions();
  List_iterator_fast<bp_t> li(m_backpatch);

  while ((bp= li++))
    if (bp->lab == lab)
    {
      sp_instr_jump *i= static_cast<sp_instr_jump *>(bp->instr);

      i->set_destination(dest);
    }
}


// ------------------------------------------------------------------

//
// sp_instr_stmt
//
int
sp_instr_stmt::execute(THD *thd, uint *nextp)
{
  LEX olex;			// The other lex
  int res;

  memcpy(&olex, &thd->lex, sizeof(LEX)); // Save the other lex

  memcpy(&thd->lex, &m_lex, sizeof(LEX)); // Use my own lex
  thd->lex.thd = thd;

  res= mysql_execute_command(thd);

  memcpy(&thd->lex, &olex, sizeof(LEX)); // Restore the other lex

  *nextp = m_ip+1;
  return res;
}

//
// sp_instr_set
//
int
sp_instr_set::execute(THD *thd, uint *nextp)
{
  thd->spcont->set_item(m_offset, eval_func_item(thd, m_value, m_type));
  *nextp = m_ip+1;
  return 0;
}

//
// sp_instr_jump_if
//
int
sp_instr_jump_if::execute(THD *thd, uint *nextp)
{
  Item *it= eval_func_item(thd, m_expr, MYSQL_TYPE_TINY);

  if (it->val_int())
    *nextp = m_dest;
  else
    *nextp = m_ip+1;
  return 0;
}

//
// sp_instr_jump_if_not
//
int
sp_instr_jump_if_not::execute(THD *thd, uint *nextp)
{
  Item *it= eval_func_item(thd, m_expr, MYSQL_TYPE_TINY);

  if (! it->val_int())
    *nextp = m_dest;
  else
    *nextp = m_ip+1;
  return 0;
}
