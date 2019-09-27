/*
 * CtlPortResolver.cpp
 *
 *  Created on: 28 сент. 2019 г.
 *      Author: sadko
 */

#include <ui/ctl/ctl.h>

namespace lsp
{
    namespace ctl
    {
        CtlPortResolver::CtlPortResolver()
        {
            pRegistry   = NULL;
        }
        
        CtlPortResolver::~CtlPortResolver()
        {
            pRegistry   = NULL;
        }
        
        void CtlPortResolver::init(CtlRegistry *registry)
        {
            pRegistry   = registry;
        }

        status_t CtlPortResolver::resolve(calc::value_t *value, const char *name, size_t num_indexes, const ssize_t *indexes)
        {
            LSPString path;
            if (!path.set_utf8(name))
                return STATUS_NO_MEM;
            for (size_t i=0; i<num_indexes; ++i)
                if (!path.fmt_append_utf8("_%d", int(indexes[i])))
                    return STATUS_NO_MEM;

            CtlPort *p      = (pRegistry != NULL) ? pRegistry->port(path.get_utf8()) : NULL;
            value->type     = (p != NULL) ? calc::VT_FLOAT : calc::VT_UNDEF;
            value->v_float  = (p != NULL) ? p->get_value() : 0.0f;
            return STATUS_OK;
        }

        status_t CtlPortResolver::resolve(calc::value_t *value, const LSPString *name, size_t num_indexes, const ssize_t *indexes)
        {
            LSPString path;
            if (num_indexes > 0)
            {
                if (!path.set(name))
                    return STATUS_NO_MEM;
                for (size_t i=0; i<num_indexes; ++i)
                    if (!path.fmt_append_utf8("_%d", int(indexes[i])))
                        return STATUS_NO_MEM;
                name = &path;
            }

            CtlPort *p      = (pRegistry != NULL) ? pRegistry->port(name->get_utf8()) : NULL;
            value->type     = (p != NULL) ? calc::VT_FLOAT : calc::VT_UNDEF;
            value->v_float  = (p != NULL) ? p->get_value() : 0.0f;
            return STATUS_OK;
        }
    
    } /* namespace java */
} /* namespace lsp */
