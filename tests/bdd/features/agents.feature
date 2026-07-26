Feature: Agent Primitives
  As a user
  I want to define agents and trigger them via @mentions in comments
  So that AI agents can inspect tasks and add comments or update descriptions

  Background:
    Given a board with a card "test" in "To Do"
    And an agent "analytical" of type "comment"

  Scenario: Unknown @mention is ignored
    When I launch the application
    And I press Enter
    And I press "c"
    And I type "@nonexistent check this"
    And I press Enter
    Then the screen should contain "No agent named"

  Scenario: Listing agents via CLI
    When I run "agents" with no arguments
    Then the exit code should be 0
    And the output should contain "analytical"

  Scenario: CLI comment with @mention triggers agent
    When I run "comment" with arguments "1" "@analytical check this"
    Then the exit code should be 0
