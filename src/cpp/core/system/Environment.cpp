/*
 * Environment.cpp
 *
 * Copyright (C) 2009-11 by RStudio, Inc.
 *
 * This program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <core/system/Environment.hpp>

#include <algorithm>

#include <boost/bind.hpp>

#ifdef _WIN32
#define kPathSeparator ";"
#else
#define kPathSeparator ":"
#endif

namespace core {
namespace system {

// get an environment variable within an Options structure
std::string getenv(const std::string& name, const Options& environment)
{
   Options::const_iterator it = std::find_if(
                                 environment.begin(),
                                 environment.end(),
                                 boost::bind(optionIsNamed, _1, name));

   if (it != environment.end())
      return it->second;
   else
      return std::string();
}

// set an environment variable within an Options structure (replaces
// any existing value)
void setenv(const std::string& name,
            const std::string& value,
            Options* pEnvironment)
{
   Options::iterator it = std::find_if(
                                 pEnvironment->begin(),
                                 pEnvironment->end(),
                                 boost::bind(optionIsNamed, _1, name));
   if (it != pEnvironment->end())
      *it = std::make_pair(name, value);
   else
      pEnvironment->push_back(std::make_pair(name, value));
}

// remove an enviroment variable from an Options structure
void unsetenv(const std::string& name, Options* pEnvironment)
{
   pEnvironment->erase(std::remove_if(pEnvironment->begin(),
                                      pEnvironment->end(),
                                      boost::bind(optionIsNamed, _1, name)));
}

// add to the PATH within an Options struture
void addToPath(const std::string& filePath, Options* pEnvironment)
{
   std::string path = getenv("PATH", *pEnvironment);
   path = path + kPathSeparator + filePath;
   setenv("PATH", path, pEnvironment);
}

bool parseEnvVar(const std::string envVar, Option* pEnvVar)
{
   std::string::size_type pos = envVar.find("=") ;
   if ( pos != std::string::npos )
   {
      std::string key = envVar.substr(0, pos) ;
      std::string value;
      if ( (pos + 1) < envVar.size() )
         value = envVar.substr(pos + 1) ;
      *pEnvVar = std::make_pair(key,value);
      return true;
   }
   else
   {
      return false;
   }
}

} // namespace system
} // namespace core
